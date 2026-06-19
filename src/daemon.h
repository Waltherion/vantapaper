#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QList>

#include <memory>
#include <vector>

#include "playlist.h"
#include "transition.h"

class QVulkanInstance;
class QLocalServer;
class WallpaperOutput;

// The running vantapaper daemon: owns one WallpaperOutput per screen, the playlist,
// the autorotation timer and the IPC server that vantapaperctl talks to.
class Daemon : public QObject
{
    Q_OBJECT
public:
    Daemon(QVulkanInstance *inst, const QString &dir, QObject *parent = nullptr);
    ~Daemon() override;

    void start(); // load config, create outputs, show the first image, serve IPC

    static QString socketPath();

private:
    void loadConfig();
    void createOutputs();
    void pollHdrStates(); // ask Hyprland for each monitor's HDR/SDR preset
    void showCurrent();
    void updateStateLinks(const QString &imagePath);
    Transition pickTransition() const;

    void next();
    void previous();
    void reload();
    void setPaused(bool paused);

    QString handleCommand(const QString &cmd);
    void startIpc();

    QVulkanInstance *m_inst = nullptr;
    QString m_dir;

    // From config (~/.config/vantapaper/config.jsonc), env can override for testing.
    int m_durationSecs = 180;
    bool m_paused = true;
    bool m_sortRandom = false; // false = ascending, true = shuffle-bag
    QList<int> m_enabledTransitions { 0, 1, 2, 3, 4 }; // fade, wipe, grow, slide, shrink
    int m_transitionMs = 600;

    Playlist m_playlist;
    std::vector<std::unique_ptr<WallpaperOutput>> m_outputs;
    QTimer m_timer;
    QTimer m_hdrPoll;
    QLocalServer *m_server = nullptr;
    quint64 m_decodeGen = 0; // newest async-decode request; stale results are dropped
};
