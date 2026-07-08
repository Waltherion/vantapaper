#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// The ordered set of wallpaper image files in a directory. The file list is always
// kept sorted case-sensitively by filename (Unicode code-unit order), which matches
// wpaperd's ascending LC_ALL=C byte sort for the ASCII filenames used here -- so the
// theme picker's "first wallpaper" prediction stays valid.
//
// Two playback orders:
//   Ascending -- step linearly through the sorted list (next wraps at the end).
//   Random    -- a shuffle-bag: every image shows once before any repeats, the bag
//                is reshuffled when exhausted, and never repeats across the seam.
//                previous() walks back through the actual visit history.
class Playlist
{
public:
    enum Mode { Ascending, Random };

    void load(const QString &dir);
    void reload();        // re-scan, keeping position on the same file if it still exists
    void setMode(Mode m); // switch playback order (resets random history/bag)

    bool isEmpty() const { return m_files.isEmpty(); }
    int size() const { return m_files.size(); }

    QString current() const;
    QString next();
    QString previous();

    // Jump to a specific file (absolute path). Returns false if it's not in the list.
    bool setCurrentPath(const QString &path);

    // True if the path's extension marks a video wallpaper (single source of truth
    // for the daemon's still/video dispatch).
    static bool isVideoPath(const QString &path);

private:
    int drawRandomIndex(); // pull the next index from the shuffle-bag (no immediate repeat)
    void resetHistory();   // history := { current }, bag cleared

    QString m_dir;
    QStringList m_files; // absolute paths, ascending
    int m_index = 0;

    Mode m_mode = Ascending;
    QVector<int> m_history; // indices visited, for Random previous/next navigation
    int m_histPos = -1;
    QVector<int> m_bag;     // remaining shuffled indices for Random next()
};
