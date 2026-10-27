#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include "fileSystemUtils.h"
#include "folderSizeCache.h"

namespace {

// Post-order walk: every subfolder lands in @p results before its parent.
// Links and junctions are not followed — junctions like "Application Data"
// point back into the profile and would loop. Unreadable subfolders are
// skipped silently, like Explorer does.
FolderStats walk(const QString &dir, const QPromise<QVector<FolderSize>> &promise,
                 std::atomic<qint64> &bytesSoFar, QVector<FolderSize> &results)
{
    FolderStats stats;

    QDirIterator it(dir, kAllEntries);
    while (it.hasNext()) {
        if (promise.isCanceled())
            return stats;   // incomplete — not recorded

        it.next();
        const QFileInfo entry = it.fileInfo();
        // isSymbolicLink(), not isLink(): a shortcut (.lnk) is a plain file here
        // and counts, as in Explorer.
        if (entry.isSymbolicLink() || entry.isJunction())
            continue;

        if (entry.isDir()) {
            const FolderStats sub = walk(entry.filePath(), promise, bytesSoFar, results);
            stats.bytes   += sub.bytes;
            stats.files   += sub.files;
            stats.folders += sub.folders + 1;
        } else {
            stats.bytes += entry.size();
            ++stats.files;
            bytesSoFar  += entry.size();
        }
    }

    results.append(FolderSize{dir, stats});
    return stats;
}

}

FolderSizeCache::FolderSizeCache(QObject *parent)
    : QObject(parent)
{}

QString FolderSizeCache::key(const QString &path)
{
    return pathKey(path);
}

bool FolderSizeCache::contains(const QString &path) const
{
    return m_stats.contains(key(path));
}

std::optional<FolderStats> FolderSizeCache::stats(const QString &path) const
{
    const auto it = m_stats.constFind(key(path));
    if (it == m_stats.cend())
        return std::nullopt;
    return *it;
}

void FolderSizeCache::insert(const QVector<FolderSize> &sizes)
{
    for (const FolderSize &size : sizes)
        m_stats.insert(key(size.path), size.stats);
    emit changed();
}

void FolderSizeCache::remove(const QString &path)
{
    if (m_stats.remove(key(path)))
        emit changed();
}

void FolderSizeCache::invalidate(const QString &path)
{
    const QString target       = key(path);
    const QString targetPrefix = target.endsWith('/') ? target : target + '/';
    bool removed = false;

    for (auto it = m_stats.begin(); it != m_stats.end();) {
        const QString &entry       = it.key();
        const QString  entryPrefix = entry.endsWith('/') ? entry : entry + '/';
        // The folder itself, everything below it, and every ancestor.
        if (entry == target || entry.startsWith(targetPrefix) || target.startsWith(entryPrefix)) {
            it = m_stats.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }

    if (removed)
        emit changed();
}

void FolderSizeCache::calculate(QPromise<QVector<FolderSize>> &promise, const QStringList &folders,
                                std::shared_ptr<std::atomic<qint64>> bytesSoFar)
{
    for (const QString &folder : folders) {
        QVector<FolderSize> results;
        walk(folder, promise, *bytesSoFar, results);
        if (promise.isCanceled())
            return;
        promise.addResult(results);
    }
}
