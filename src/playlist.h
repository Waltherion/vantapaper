#pragma once

#include <QString>
#include <QStringList>

// The ordered set of wallpaper image files in a directory. Sorted case-sensitively
// by filename (Unicode code-unit order), which matches wpaperd's ascending
// LC_ALL=C byte sort for the ASCII filenames used here -- so the theme picker's
// "first wallpaper" prediction stays valid after the switch to vantapaper.
class Playlist
{
public:
    void load(const QString &dir);
    void reload(); // re-scan, keeping position on the same file if it still exists

    bool isEmpty() const { return m_files.isEmpty(); }
    int size() const { return m_files.size(); }

    QString current() const;
    QString next();
    QString previous();

    // Jump to a specific file (absolute path). Returns false if it's not in the list.
    bool setCurrentPath(const QString &path);

private:
    QString m_dir;
    QStringList m_files; // absolute paths
    int m_index = 0;
};
