#include "playlist.h"

#include <QDir>
#include <QRandomGenerator>
#include <algorithm>

static const QStringList kExtensions = {
    QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
    QStringLiteral("avif"), QStringLiteral("jxl"),  QStringLiteral("heic"),
    QStringLiteral("heif"), QStringLiteral("webp"), QStringLiteral("tiff"),
    QStringLiteral("tif"),  QStringLiteral("bmp"),
};

void Playlist::load(const QString &dir)
{
    m_dir = dir;
    m_index = 0;
    reload();
}

void Playlist::setMode(Mode m)
{
    if (m_mode == m)
        return;
    m_mode = m;
    resetHistory();
}

void Playlist::resetHistory()
{
    m_history.clear();
    if (!m_files.isEmpty())
        m_history.append(qBound(0, m_index, m_files.size() - 1));
    m_histPos = m_history.isEmpty() ? -1 : 0;
    m_bag.clear();
}

void Playlist::reload()
{
    const QString keep = current(); // remember the file we're on, to stay on it

    QDir d(m_dir);
    QStringList names;
    for (const QString &n : d.entryList(QDir::Files, QDir::NoSort)) {
        const int dot = n.lastIndexOf(QLatin1Char('.'));
        if (dot >= 0 && kExtensions.contains(n.mid(dot + 1).toLower()))
            names << n;
    }
    std::sort(names.begin(), names.end()); // code-unit order == LC_ALL=C for ASCII

    m_files.clear();
    for (const QString &n : names)
        m_files << d.absoluteFilePath(n);

    if (!keep.isEmpty()) {
        const int i = m_files.indexOf(keep);
        m_index = (i >= 0) ? i : 0;
    } else {
        m_index = 0;
    }
    // File set may have changed -- old history/bag indices are no longer valid.
    resetHistory();
}

QString Playlist::current() const
{
    if (m_files.isEmpty())
        return QString();
    return m_files.at(qBound(0, m_index, m_files.size() - 1));
}

int Playlist::drawRandomIndex()
{
    const int n = m_files.size();
    if (n <= 1)
        return 0;
    if (m_bag.isEmpty()) {
        m_bag.resize(n);
        for (int i = 0; i < n; ++i)
            m_bag[i] = i;
        auto *rng = QRandomGenerator::global();
        for (int i = n - 1; i > 0; --i) // Fisher-Yates
            std::swap(m_bag[i], m_bag[rng->bounded(i + 1)]);
        // next() pulls from the back; make sure that isn't the image we're on now.
        if (m_bag.last() == m_index)
            std::swap(m_bag.first(), m_bag.last());
    }
    return m_bag.takeLast();
}

QString Playlist::next()
{
    if (m_files.isEmpty())
        return QString();
    if (m_mode == Ascending) {
        m_index = (m_index + 1) % m_files.size();
        return current();
    }
    // Random: redo forward through history if we'd stepped back; else draw fresh.
    if (m_histPos >= 0 && m_histPos < m_history.size() - 1) {
        m_index = m_history.at(++m_histPos);
    } else {
        m_index = drawRandomIndex();
        m_history.append(m_index);
        if (m_history.size() > 512)
            m_history.removeFirst();
        m_histPos = m_history.size() - 1;
    }
    return current();
}

QString Playlist::previous()
{
    if (m_files.isEmpty())
        return QString();
    if (m_mode == Ascending) {
        m_index = (m_index - 1 + m_files.size()) % m_files.size();
        return current();
    }
    // Random: walk back through the actual visit history (can't go before the first
    // image shown this session).
    if (m_histPos > 0)
        m_index = m_history.at(--m_histPos);
    return current();
}

bool Playlist::setCurrentPath(const QString &path)
{
    const int i = m_files.indexOf(path);
    if (i < 0)
        return false;
    m_index = i;
    if (m_mode == Random)
        resetHistory(); // a deliberate jump becomes the new history root
    return true;
}
