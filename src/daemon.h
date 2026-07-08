#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QList>
#include <QHash>

#include <memory>
#include <vector>

#include "playlist.h"
#include "transition.h"

class QVulkanInstance;
class QLocalServer;
class WallpaperOutput;

// The running vantapaper daemon: owns one WallpaperOutput per screen (each with its OWN
// playlist, so monitors can show different wallpapers), the autorotation timer and the IPC
// server that vantapaperctl talks to.
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
    void showAll();             // (re)show every output's current image, random pool transition
    void showAll(Transition t); // ... with a specific transition (directional next/prev)
    void showOne(WallpaperOutput *win, const QString &path, Transition t, quint64 gen);
    // Video branch of showOne: open (hw->sw fallback) + first frame async, then
    // setVideo; the state link points at a cached PNG of the first frame.
    void showVideo(WallpaperOutput *win, const QString &path, Transition t, quint64 gen);
    // linkTarget = decodable image for the lockscreen; sourcePath (when different)
    // is recorded in a "<output>.source" sidecar so tools can see the real file.
    void updateStateLink(const QString &name, const QString &linkTarget,
                         const QString &sourcePath = QString());
    Transition pickTransition() const;          // a random transition from the enabled pool
    Transition resolveSpec(const TransitionSpec &s) const; // spec -> Transition (randomised fields)

    void next();
    void previous();
    void reload();
    void setPaused(bool paused);

    QString handleCommand(const QString &cmd);
    void startIpc();

    QVulkanInstance *m_inst = nullptr;
    QString m_dir; // global default source folder (per-output overrides in m_outputDirs)
    QHash<QString, QString> m_outputDirs; // screen name -> source folder override (from config "outputs")

    // Filmstrip mode: ties next/previous (and autorotation) to a directional slide.
    enum class Series { None, Horizontal, Vertical };

    // From config (~/.config/vantapaper/config.jsonc), env can override for testing.
    int m_durationSecs = 180;
    bool m_videoHwdec = true; // "video": { "hwdec": "auto"|"off" }
    bool m_paused = true;
    bool m_sortRandom = false; // false = ascending, true = shuffle-bag
    QList<TransitionSpec> m_enabledTransitions {
        {0, -1}, {1, -1}, {2, -1}, {3, -1}, {4, -1} }; // fade, wipe, grow, slide, shrink
    Series m_series = Series::None;
    int m_transitionMs = 600;

    // One WallpaperOutput per screen, each with its own playlist (so monitors differ).
    struct OutputState {
        std::unique_ptr<WallpaperOutput> win;
        Playlist playlist;
    };
    std::vector<OutputState> m_outputs;
    QTimer m_timer;
    QTimer m_hdrPoll;
    QLocalServer *m_server = nullptr;
    quint64 m_decodeGen = 0; // newest async-decode request; stale results are dropped
};
