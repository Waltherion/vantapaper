#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

#include "playlist.h"

class QVulkanInstance;
class QLocalServer;
class WallpaperOutput;

// The running vantapaper daemon: owns one WallpaperOutput per screen, the playlist,
// the autorotation timer and the IPC server that vantapaperctl talks to.
class Daemon : public QObject
{
    Q_OBJECT
public:
    Daemon(QVulkanInstance *inst, const QString &dir, int durationSecs, bool startPaused,
           QObject *parent = nullptr);
    ~Daemon() override;

    void start(); // create outputs, show the first image, begin serving IPC

    static QString socketPath();

private:
    void createOutputs();
    void showCurrent();
    void updateStateLinks(const QString &imagePath);

    void next();
    void previous();
    void reload();
    void setPaused(bool paused);

    QString handleCommand(const QString &cmd);
    void startIpc();

    QVulkanInstance *m_inst = nullptr;
    QString m_dir;
    int m_durationSecs = 180;
    bool m_paused = true;

    Playlist m_playlist;
    std::vector<std::unique_ptr<WallpaperOutput>> m_outputs;
    QTimer m_timer;
    QLocalServer *m_server = nullptr;
};
