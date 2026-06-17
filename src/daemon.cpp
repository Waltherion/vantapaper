#include "daemon.h"

#include "rhiwindow.h"
#include "hdr_image.h"

#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>

#include <LayerShellQt/window.h>

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

static QString stateDir()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/vantapaper/wallpapers");
}

Daemon::Daemon(QVulkanInstance *inst, const QString &dir, int durationSecs, bool startPaused,
               QObject *parent)
    : QObject(parent), m_inst(inst), m_dir(dir), m_durationSecs(durationSecs), m_paused(startPaused)
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

void Daemon::start()
{
    m_playlist.load(m_dir);
    if (m_playlist.isEmpty())
        qWarning("vantapaper: no images found in %s", qPrintable(m_dir));

    createOutputs();
    showCurrent();
    for (auto &o : m_outputs)
        o->show();

    startIpc();
    setPaused(m_paused); // arms the timer when playing

    qInfo("vantapaper: daemon up -- %zu output(s), %d image(s), duration %ds, %s",
          m_outputs.size(), m_playlist.size(), m_durationSecs, m_paused ? "paused" : "playing");
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
    for (auto &o : m_outputs)
        o->setImage(img);

    updateStateLinks(path);
    qInfo("vantapaper: showing %s", qPrintable(path));
}

void Daemon::updateStateLinks(const QString &imagePath)
{
    // A symlink per output pointing at the current image, e.g. for the lockscreen.
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
        m_timer.start(m_durationSecs * 1000); // restart the interval after a switch
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
            const QString reply = handleCommand(cmd);
            c->write(reply.toUtf8() + '\n');
            c->flush();
            c->disconnectFromServer();
        });
    });
}
