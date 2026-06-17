#include "daemon.h"

#include "rhiwindow.h"
#include "hdr_image.h"

#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QtMath>

#include <LayerShellQt/window.h>

static const char *kDefaultConfig = R"(// vantapaper configuration (JSONC: // and /* */ comments are allowed)
{
  // --- Autorotation ---
  "durationSecs": 180,   // seconds between automatic wallpaper changes
  "startPaused": true,   // start with autorotation paused (toggle with the keybind)

  // --- Transitions ---
  // A random one is chosen from "enabled" on each change.
  // Available: "fade", "wipe", "grow". Remove any you dislike.
  // Use [] or ["none"] to switch instantly with no animation.
  "transition": {
    "enabled": ["fade", "wipe", "grow"],
    "durationMs": 600
  }
}
)";

static QString configPath()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/vantapaper/config.jsonc");
}

static QString stateDir()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/vantapaper/wallpapers");
}

// Strip // and /* */ comments (respecting strings) and trailing commas, so a JSONC
// file parses with QJsonDocument.
static QByteArray cleanJsonc(const QByteArray &in)
{
    QByteArray s;
    s.reserve(in.size());
    bool inStr = false, esc = false;
    for (int i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (inStr) {
            s += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; s += c; continue; }
        if (c == '/' && i + 1 < in.size() && in.at(i + 1) == '/') {
            while (i < in.size() && in.at(i) != '\n') ++i;
            s += '\n';
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in.at(i + 1) == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in.at(i) == '*' && in.at(i + 1) == '/')) ++i;
            ++i;
            continue;
        }
        s += c;
    }
    QString str = QString::fromUtf8(s);
    str.replace(QRegularExpression(QStringLiteral(",\\s*([}\\]])")), QStringLiteral("\\1"));
    return str.toUtf8();
}

static void setupLayerShell(QWindow *w, QScreen *screen)
{
    if (LayerShellQt::Window *ls = LayerShellQt::Window::get(w)) {
        ls->setLayer(LayerShellQt::Window::LayerBackground);
        ls->setAnchors(LayerShellQt::Window::Anchors(
            LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(-1);
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        ls->setScope(QStringLiteral("vantapaper"));
        ls->setScreen(screen);
    } else {
        qWarning("vantapaper: not on a layer surface for %s", qPrintable(screen->name()));
    }
}

Daemon::Daemon(QVulkanInstance *inst, const QString &dir, QObject *parent)
    : QObject(parent), m_inst(inst), m_dir(dir)
{
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, [this]() { next(); });
}

Daemon::~Daemon() = default;

QString Daemon::socketPath()
{
    QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (rt.isEmpty())
        rt = QStringLiteral("/tmp");
    return rt + QStringLiteral("/vantapaper.sock");
}

void Daemon::loadConfig()
{
    const QString path = configPath();
    QFile f(path);
    if (!f.exists()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (f.open(QIODevice::WriteOnly)) {
            f.write(kDefaultConfig);
            f.close();
            qInfo("vantapaper: wrote default config %s", qPrintable(path));
        }
    }
    if (!f.open(QIODevice::ReadOnly))
        return; // keep defaults

    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(cleanJsonc(raw), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("vantapaper: config parse error (%s) -- using defaults",
                 qPrintable(err.errorString()));
        return;
    }

    const QJsonObject o = doc.object();
    m_durationSecs = o.value(QStringLiteral("durationSecs")).toInt(m_durationSecs);
    m_paused = o.value(QStringLiteral("startPaused")).toBool(m_paused);

    const QJsonObject tr = o.value(QStringLiteral("transition")).toObject();
    m_transitionMs = tr.value(QStringLiteral("durationMs")).toInt(m_transitionMs);
    if (tr.value(QStringLiteral("enabled")).isArray()) {
        m_enabledTransitions.clear();
        for (const QJsonValue &v : tr.value(QStringLiteral("enabled")).toArray()) {
            const QString name = v.toString().toLower();
            if (name == QLatin1String("fade")) m_enabledTransitions << 0;
            else if (name == QLatin1String("wipe")) m_enabledTransitions << 1;
            else if (name == QLatin1String("grow")) m_enabledTransitions << 2;
            else if (name == QLatin1String("none")) m_enabledTransitions << -1;
        }
    }
}

void Daemon::start()
{
    loadConfig();

    // Env overrides for testing.
    bool okDur = false;
    const int d = qEnvironmentVariable("VANTAPAPER_DURATION_SECS").toInt(&okDur);
    if (okDur && d > 0)
        m_durationSecs = d;
    if (qEnvironmentVariable("VANTAPAPER_START_PLAYING") == QLatin1String("1"))
        m_paused = false;

    m_playlist.load(m_dir);
    if (m_playlist.isEmpty())
        qWarning("vantapaper: no images found in %s", qPrintable(m_dir));

    createOutputs();
    showCurrent();
    for (auto &o : m_outputs)
        o->show();

    startIpc();
    setPaused(m_paused);

    qInfo("vantapaper: daemon up -- %zu output(s), %d image(s), duration %ds, %s, %d transition(s)",
          m_outputs.size(), m_playlist.size(), m_durationSecs, m_paused ? "paused" : "playing",
          int(m_enabledTransitions.size()));
}

void Daemon::createOutputs()
{
    for (QScreen *screen : QGuiApplication::screens()) {
        auto w = std::make_unique<WallpaperOutput>(m_inst, screen);
        setupLayerShell(w.get(), screen);
        w->resize(screen->size());
        qInfo("vantapaper: output on %s (%dx%d)", qPrintable(screen->name()),
              screen->size().width(), screen->size().height());
        m_outputs.push_back(std::move(w));
    }
}

Transition Daemon::pickTransition() const
{
    Transition t;
    t.durationMs = m_transitionMs;
    if (m_enabledTransitions.isEmpty()) {
        t.type = -1;
        return t;
    }
    auto *rng = QRandomGenerator::global();
    t.type = m_enabledTransitions.at(rng->bounded(m_enabledTransitions.size()));
    if (t.type == 1) {
        // wipe: usually a clean cardinal edge, sometimes an angled one.
        if (rng->bounded(10) < 7)
            t.angle = float(rng->bounded(4)) * float(M_PI_2);
        else
            t.angle = float(rng->bounded(2.0 * M_PI));
    } else if (t.type == 2) {
        // grow: a random-ish centre, biased away from the very edges.
        t.cx = 0.2f + float(rng->bounded(0.6));
        t.cy = 0.2f + float(rng->bounded(0.6));
    }
    return t;
}

void Daemon::showCurrent()
{
    const QString path = m_playlist.current();
    if (path.isEmpty())
        return;

    HdrImage h = decodeImage(path);
    if (!h.valid()) {
        qWarning("vantapaper: failed to decode %s", qPrintable(path));
        return;
    }
    auto img = std::make_shared<const HdrImage>(std::move(h));
    const Transition t = pickTransition(); // ignored by an output until its first image
    for (auto &o : m_outputs)
        o->setImage(img, t);

    updateStateLinks(path);
    qInfo("vantapaper: showing %s", qPrintable(path));
}

void Daemon::updateStateLinks(const QString &imagePath)
{
    const QString dir = stateDir();
    QDir().mkpath(dir);
    for (auto &o : m_outputs) {
        const QString name = o->screenName();
        if (name.isEmpty())
            continue;
        const QString link = dir + QLatin1Char('/') + name;
        QFile::remove(link);
        QFile::link(imagePath, link);
    }
}

void Daemon::next()
{
    m_playlist.next();
    showCurrent();
    if (!m_paused)
        m_timer.start(m_durationSecs * 1000);
}

void Daemon::previous()
{
    m_playlist.previous();
    showCurrent();
    if (!m_paused)
        m_timer.start(m_durationSecs * 1000);
}

void Daemon::reload()
{
    m_playlist.reload();
    showCurrent();
}

void Daemon::setPaused(bool paused)
{
    m_paused = paused;
    if (m_paused)
        m_timer.stop();
    else
        m_timer.start(m_durationSecs * 1000);
}

QString Daemon::handleCommand(const QString &cmd)
{
    if (cmd == QLatin1String("next")) { next(); return QStringLiteral("ok"); }
    if (cmd == QLatin1String("previous") || cmd == QLatin1String("prev")) { previous(); return QStringLiteral("ok"); }
    if (cmd == QLatin1String("reload")) { reload(); return QStringLiteral("ok"); }
    if (cmd == QLatin1String("pause")) { setPaused(true); return QStringLiteral("paused"); }
    if (cmd == QLatin1String("play")) { setPaused(false); return QStringLiteral("playing"); }
    if (cmd == QLatin1String("toggle-pause")) { setPaused(!m_paused); return m_paused ? QStringLiteral("paused") : QStringLiteral("playing"); }
    if (cmd == QLatin1String("status")) { return m_paused ? QStringLiteral("paused") : QStringLiteral("playing"); }
    return QStringLiteral("unknown command: ") + cmd;
}

void Daemon::startIpc()
{
    QLocalServer::removeServer(socketPath());
    m_server = new QLocalServer(this);
    if (!m_server->listen(socketPath())) {
        qWarning("vantapaper: IPC listen failed on %s: %s", qPrintable(socketPath()),
                 qPrintable(m_server->errorString()));
        return;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        QLocalSocket *c = m_server->nextPendingConnection();
        connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        connect(c, &QLocalSocket::readyRead, this, [this, c]() {
            const QString cmd = QString::fromUtf8(c->readLine()).trimmed();
            c->write(handleCommand(cmd).toUtf8() + '\n');
            c->flush();
            c->disconnectFromServer();
        });
    });
}
