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
#include <QProcess>
#include <QHash>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QtMath>

#include <LayerShellQt/window.h>

static const char *kDefaultConfig = R"(// vantapaper configuration (JSONC: // and /* */ comments are allowed)
{
  // --- Source ---
  // Folder to read wallpapers from. "~" and $ENV are expanded.
  "path": "~/Pictures/wallpapers",

  // --- Autorotation ---
  "durationSecs": 180,   // seconds between automatic wallpaper changes
  "startPaused": true,   // start with autorotation paused (toggle with the keybind)

  // --- Playback order ---
  // "ascending" -> alphabetical by filename. "random" -> shuffle-bag: every image
  // shows once before any repeats, never repeating across a reshuffle.
  "sort": "ascending",

  // --- Transitions ---
  // A random one is chosen from "enabled" on each change. Available: "fade", "wipe",
  // "grow", "shrink", "slide", "wave", "pixelate", "blinds", and "radial" (alias
  // "clock"). Slide may be plain ("slide" = random side) or directional:
  // "slide-left"/"slide-right"/"slide-up"/"slide-down" name the side the new image
  // enters from. Use [] or ["none"] to switch instantly with no animation.
  //
  // "series" ties next/previous (and autorotation) to a directional slide, so a run of
  // images reads like a filmstrip:
  //   "none"        -> next/previous use the random pool above
  //   "horizontal"  -> next enters from the right, previous from the left
  //   "vertical"    -> next enters from the bottom, previous from the top
  "transition": {
    "enabled": ["fade", "wipe", "slide", "grow", "shrink", "wave", "pixelate", "blinds", "radial"],
    "series": "none",
    "durationMs": 600
  }
}
)";

// Expand a leading "~" and $ENV references in a config path.
static QString expandPath(QString p)
{
    if (p == QLatin1String("~"))
        return QDir::homePath();
    if (p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    QRegularExpression re(QStringLiteral("\\$(\\w+)|\\$\\{(\\w+)\\}"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(p);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += p.mid(last, m.capturedStart() - last);
        const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        out += qEnvironmentVariable(name.toLocal8Bit().constData());
        last = m.capturedEnd();
    }
    out += p.mid(last);
    return out;
}

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

// File that remembers play/paused across restarts (theme switches restart us).
static QString playStatePath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/vantapaper/play-state");
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

// Side the new image enters from -> slide direction angle (radians).
// 0 left, 1 right, 2 top, 3 bottom. (Verified against shaders/image.frag's slide branch.)
static float angleForSide(int side)
{
    switch (side) {
    case 1:  return float(M_PI);         // enters from the right
    case 2:  return float(M_PI_2);       // enters from the top
    case 3:  return float(3.0 * M_PI_2); // enters from the bottom
    case 0:
    default: return 0.0f;                // enters from the left
    }
}

// Map a config transition name to a spec. type == -2 -> unrecognised (caller skips it).
static TransitionSpec parseTransitionName(const QString &raw)
{
    const QString n = raw.trimmed().toLower();
    if (n == QLatin1String("fade"))        return { 0, -1 };
    if (n == QLatin1String("wipe"))        return { 1, -1 };
    if (n == QLatin1String("grow"))        return { 2, -1 };
    if (n == QLatin1String("shrink"))      return { 4, -1 };
    if (n == QLatin1String("none"))        return { -1, -1 };
    if (n == QLatin1String("slide"))       return { 3, -1 };
    if (n == QLatin1String("slide-left"))  return { 3, 0 };
    if (n == QLatin1String("slide-right")) return { 3, 1 };
    if (n == QLatin1String("slide-up"))    return { 3, 2 };
    if (n == QLatin1String("slide-down"))  return { 3, 3 };
    if (n == QLatin1String("wave"))        return { 5, -1 };
    if (n == QLatin1String("pixelate"))    return { 6, -1 };
    if (n == QLatin1String("blinds"))      return { 7, -1 };
    if (n == QLatin1String("radial"))      return { 8, -1 };
    if (n == QLatin1String("clock"))       return { 8, -1 }; // alias for radial
    return { -2, -1 };
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
    const QString cfgPath = o.value(QStringLiteral("path")).toString();
    if (!cfgPath.isEmpty())
        m_dir = expandPath(cfgPath);
    m_durationSecs = o.value(QStringLiteral("durationSecs")).toInt(m_durationSecs);
    m_paused = o.value(QStringLiteral("startPaused")).toBool(m_paused);
    m_sortRandom = o.value(QStringLiteral("sort")).toString(
                       m_sortRandom ? QStringLiteral("random") : QStringLiteral("ascending"))
                       .toLower() == QLatin1String("random");

    const QJsonObject tr = o.value(QStringLiteral("transition")).toObject();
    m_transitionMs = tr.value(QStringLiteral("durationMs")).toInt(m_transitionMs);
    if (tr.value(QStringLiteral("enabled")).isArray()) {
        m_enabledTransitions.clear();
        for (const QJsonValue &v : tr.value(QStringLiteral("enabled")).toArray()) {
            const TransitionSpec s = parseTransitionName(v.toString());
            if (s.type != -2)
                m_enabledTransitions << s;
        }
    }

    // Filmstrip mode: tie next/previous (and autorotation) to a directional slide.
    const QString series = tr.value(QStringLiteral("series")).toString().toLower();
    if (series == QLatin1String("horizontal"))    m_series = Series::Horizontal;
    else if (series == QLatin1String("vertical")) m_series = Series::Vertical;
    else                                          m_series = Series::None;
}

void Daemon::start()
{
    loadConfig();

    // Restore the last play/paused state (survives restarts + theme switches),
    // overriding the config default.
    {
        QFile f(playStatePath());
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray s = f.readAll().trimmed();
            if (s == "paused")
                m_paused = true;
            else if (s == "playing")
                m_paused = false;
        }
    }

    // Env overrides for testing (highest precedence).
    bool okDur = false;
    const int d = qEnvironmentVariable("VANTAPAPER_DURATION_SECS").toInt(&okDur);
    if (okDur && d > 0)
        m_durationSecs = d;
    if (qEnvironmentVariable("VANTAPAPER_START_PLAYING") == QLatin1String("1"))
        m_paused = false;

    m_playlist.load(m_dir);
    m_playlist.setMode(m_sortRandom ? Playlist::Random : Playlist::Ascending);
    if (m_playlist.isEmpty())
        qWarning("vantapaper: no images found in %s", qPrintable(m_dir));

    createOutputs();
    pollHdrStates(); // set each output's initial HDR/SDR mode before first render
    showCurrent();
    for (auto &o : m_outputs)
        o->show();

    // Poll for HDR/SDR toggles (e.g. Super+Ctrl+H) so outputs re-adapt live.
    connect(&m_hdrPoll, &QTimer::timeout, this, &Daemon::pollHdrStates);
    m_hdrPoll.start(2000);

    startIpc();
    setPaused(m_paused);

    qInfo("vantapaper: daemon up -- %zu output(s), %d image(s), duration %ds, %s, %s, %d transition(s)",
          m_outputs.size(), m_playlist.size(), m_durationSecs, m_paused ? "paused" : "playing",
          m_sortRandom ? "random" : "ascending", int(m_enabledTransitions.size()));
}

void Daemon::pollHdrStates()
{
    // Hyprland reports each monitor's colour-management preset ("hdr"/"srgb"),
    // which flips when the user toggles HDR. Cheaper + more reliable than the
    // wp-color-management surface-feedback dance, and vantapaper is Hyprland-only.
    QProcess p;
    p.start(QStringLiteral("hyprctl"), { QStringLiteral("monitors"), QStringLiteral("-j") });
    if (!p.waitForFinished(500)) {
        p.kill();
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
    if (!doc.isArray())
        return;

    QHash<QString, bool> hdrByName;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject m = v.toObject();
        hdrByName.insert(m.value(QStringLiteral("name")).toString(),
                         m.value(QStringLiteral("colorManagementPreset")).toString()
                             == QLatin1String("hdr"));
    }
    for (auto &o : m_outputs) {
        const QString name = o->screenName();
        if (hdrByName.contains(name))
            o->setHdrMode(hdrByName.value(name));
    }
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

Transition Daemon::resolveSpec(const TransitionSpec &s) const
{
    Transition t;
    t.type = s.type;
    t.durationMs = m_transitionMs;
    auto *rng = QRandomGenerator::global();
    if (t.type == 1 || t.type == 5) {
        // wipe / wave: usually a clean cardinal edge, sometimes an angled one.
        if (rng->bounded(10) < 7)
            t.angle = float(rng->bounded(4)) * float(M_PI_2);
        else
            t.angle = float(rng->bounded(2.0 * M_PI));
    } else if (t.type == 2 || t.type == 4 || t.type == 8) {
        // grow / shrink / radial: a random-ish centre, biased away from the very edges.
        t.cx = 0.2f + float(rng->bounded(0.6));
        t.cy = 0.2f + float(rng->bounded(0.6));
    } else if (t.type == 7) {
        // blinds: horizontal or vertical slats.
        t.angle = (rng->bounded(2) == 0) ? 0.0f : float(M_PI_2);
    } else if (t.type == 3) {
        // slide/push: a fixed side, or a random cardinal when slideSide < 0.
        const int side = (s.slideSide >= 0) ? s.slideSide : rng->bounded(4);
        t.angle = angleForSide(side);
    }
    return t;
}

Transition Daemon::pickTransition() const
{
    if (m_enabledTransitions.isEmpty()) {
        Transition t;
        t.type = -1;
        t.durationMs = m_transitionMs;
        return t;
    }
    auto *rng = QRandomGenerator::global();
    return resolveSpec(m_enabledTransitions.at(rng->bounded(m_enabledTransitions.size())));
}

void Daemon::showCurrent()
{
    showCurrent(pickTransition());
}

void Daemon::showCurrent(Transition t)
{
    const QString path = m_playlist.current();
    if (path.isEmpty())
        return;

    // Decode off the main thread so large images don't freeze the switch. The
    // current wallpaper stays on screen until the new one is ready; only the
    // newest request wins (rapid next/prev decodes the final image, drops the rest).
    const quint64 gen = ++m_decodeGen;
    auto *watcher = new QFutureWatcher<std::shared_ptr<const HdrImage>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path, gen, t]() {
        const std::shared_ptr<const HdrImage> img = watcher->result();
        watcher->deleteLater();
        if (gen != m_decodeGen)
            return; // superseded by a newer switch
        if (!img) {
            qWarning("vantapaper: failed to decode %s", qPrintable(path));
            return;
        }
        for (auto &o : m_outputs)
            o->setImage(img, t); // t ignored until an output's first image
        updateStateLinks(path);
        qInfo("vantapaper: showing %s", qPrintable(path));
    });
    watcher->setFuture(QtConcurrent::run([path]() -> std::shared_ptr<const HdrImage> {
        HdrImage h = decodeImage(path);
        if (!h.valid())
            return {};
        return std::make_shared<const HdrImage>(std::move(h));
    }));
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
    // Filmstrip: advance enters from the right (horizontal) / bottom (vertical).
    if (m_series == Series::Horizontal)    showCurrent(resolveSpec({ 3, 1 }));
    else if (m_series == Series::Vertical) showCurrent(resolveSpec({ 3, 3 }));
    else                                   showCurrent();
    if (!m_paused)
        m_timer.start(m_durationSecs * 1000);
}

void Daemon::previous()
{
    m_playlist.previous();
    // Filmstrip: go back enters from the left (horizontal) / top (vertical).
    if (m_series == Series::Horizontal)    showCurrent(resolveSpec({ 3, 0 }));
    else if (m_series == Series::Vertical) showCurrent(resolveSpec({ 3, 2 }));
    else                                   showCurrent();
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

    // Persist so the choice survives restarts + theme switches.
    QDir().mkpath(QFileInfo(playStatePath()).absolutePath());
    QFile f(playStatePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(m_paused ? "paused\n" : "playing\n");
}

QString Daemon::handleCommand(const QString &cmd)
{
    if (cmd.startsWith(QLatin1String("show "))) {
        const QString path = cmd.mid(5);
        if (!m_playlist.setCurrentPath(path)) {
            m_playlist.reload(); // maybe a newly-added image
            if (!m_playlist.setCurrentPath(path))
                return QStringLiteral("not found: ") + path;
        }
        showCurrent();
        return QStringLiteral("ok");
    }
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
