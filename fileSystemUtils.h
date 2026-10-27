#pragma once

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QString>
#include <QStringList>

// Path, link and size helpers shared by the panels, the worker, the job queue
// and the folder size cache. Only isLink() reads the filesystem (via QFileInfo).

/// Path casing — Windows is case-insensitive, POSIX is case-sensitive.
inline constexpr Qt::CaseSensitivity kPathCase =
#ifdef Q_OS_WIN
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

/// Every entry of a folder, hidden / system ones included. Size scans, copy,
/// move, delete and the folder size walk must list the same entries —
/// otherwise the progress no longer adds up to 100 %.
inline constexpr QDir::Filters kAllEntries = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                             | QDir::Hidden | QDir::System;

/// @return True if @p a and @p b name the same path — compared as strings, no filesystem access.
inline bool pathEquals(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), kPathCase) == 0;
}

/// @return True if @p path is @p root itself or lies somewhere below it.
/// Works for drive roots ("C:/") and UNC roots ("//nas/media") alike.
inline bool isSameOrInside(const QString &path, const QString &root)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);
    if (cleanPath.compare(cleanRoot, kPathCase) == 0)
        return true;
    // cleanPath() keeps the slash of a root ("C:/") — don't add a second one.
    const QString prefix = cleanRoot.endsWith('/') ? cleanRoot : cleanRoot + '/';
    return cleanPath.startsWith(prefix, kPathCase);
}

/// @return Cleaned path that compares equal for the same folder (case-folded on
/// Windows) — for hash keys and string comparisons without filesystem access.
inline QString pathKey(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    return kPathCase == Qt::CaseInsensitive ? clean.toLower() : clean;
}

/// @return Number of "//server/share/…" segments, or 0 if @p path is not a UNC path.
inline int uncSegmentCount(const QString &path)
{
    const QString cleaned = QDir::fromNativeSeparators(path.trimmed());
    if (!cleaned.startsWith(QLatin1String("//")))
        return 0;
    return int(cleaned.split('/', Qt::SkipEmptyParts).size());
}

/// @return True for "\\server" alone (no share) — QFileSystemModel cannot list a server, only its shares.
inline bool isServerOnlyPath(const QString &path) { return uncSegmentCount(path) == 1; }

/// @return True for "\\server\share" or deeper.
inline bool isSharePath(const QString &path) { return uncSegmentCount(path) >= 2; }

/// @return "//server/share" of a UNC path (native separators accepted), or empty
/// if @p path has no share part.
inline QString uncShareRoot(const QString &path)
{
    if (!isSharePath(path))
        return {};
    const QStringList parts = QDir::fromNativeSeparators(path.trimmed()).split('/', Qt::SkipEmptyParts);
    return QStringLiteral("//%1/%2").arg(parts[0], parts[1]);
}

/// @return True for a symlink or an NTFS junction — never followed by copy,
/// move, delete or a size scan. Qt 6 reports junctions only via isJunction() —
/// isSymLink() is false and isDir() true for them, so an isSymLink() check
/// alone walks into the target (and a delete wipes its contents).
inline bool isLink(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

/// @return "1.2 MB" — Explorer-style units instead of Qt's default KiB / MiB.
inline QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}
