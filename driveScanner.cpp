#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <algorithm>
#include "driveScanner.h"

#ifdef Q_OS_WIN
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>
#include <winnetwk.h>
#endif

namespace {

// Fill label/size fields from a QStorageInfo that has already been read.
void applyStorageInfo(DriveInfo &drive, const QStorageInfo &storage)
{
    drive.volumeName     = storage.name();
    drive.displayName    = storage.displayName();
    drive.fileSystemType = QString::fromUtf8(storage.fileSystemType());
    drive.bytesTotal     = storage.bytesTotal();
    drive.bytesAvailable = storage.bytesAvailable();
}

DriveInfo makeLocalDrive(const QStorageInfo &storage)
{
    DriveInfo drive;
    drive.rootPath = storage.rootPath();
    drive.state    = DriveInfo::State::Ready;
    applyStorageInfo(drive, storage);
    return drive;
}

#ifdef Q_OS_WIN
constexpr int kDriveLetterCount = 26;

// Local volumes incl. folder mount points. FindFirstVolume never lists
// network shares, so nothing here can block on an unreachable server.
void appendLocalVolumes(QList<DriveInfo> &drives)
{
    wchar_t volumeName[MAX_PATH];
    const HANDLE handle = FindFirstVolumeW(volumeName, MAX_PATH);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do {
        // First call only asks for the required buffer length.
        DWORD length = 0;
        GetVolumePathNamesForVolumeNameW(volumeName, nullptr, 0, &length);
        if (length == 0)
            continue;

        std::vector<wchar_t> paths(length);
        if (!GetVolumePathNamesForVolumeNameW(volumeName, paths.data(), length, &length))
            continue;

        // Multi-string: "C:\\\0D:\\mnt\\\0\0" — volumes without a mount point yield none.
        for (const wchar_t *path = paths.data(); *path; path += std::wcslen(path) + 1) {
            const QStorageInfo storage(QString::fromWCharArray(path));
            // Not ready = e.g. empty card reader / DVD drive — hidden as before.
            if (storage.isValid() && storage.isReady())
                drives.append(makeLocalDrive(storage));
        }
    } while (FindNextVolumeW(handle, volumeName, MAX_PATH));

    FindVolumeClose(handle);
}

// UNC target of a connected drive letter ("Z:"), empty if unknown.
// Answered by the local redirector — no round trip to the server.
QString remotePathFor(const QString &letter)
{
    wchar_t buffer[MAX_PATH];
    DWORD length = MAX_PATH;
    const std::wstring localName = letter.toStdWString();
    if (WNetGetConnectionW(localName.c_str(), buffer, &length) != NO_ERROR)
        return {};
    return QString::fromWCharArray(buffer);
}

// Persistent mappings that are not connected right now ("net use /persistent",
// not restored at logon) — they have no bit in GetLogicalDrives().
void appendRememberedDrives(QList<DriveInfo> &drives, DWORD connectedMask)
{
    HANDLE handle = nullptr;
    if (WNetOpenEnumW(RESOURCE_REMEMBERED, RESOURCETYPE_DISK, 0, nullptr, &handle) != NO_ERROR)
        return;

    // 16 KiB is the buffer size recommended by the WNetEnumResource docs.
    std::vector<BYTE> buffer(16 * 1024);
    while (true) {
        DWORD count = DWORD(-1);   // as many entries as fit
        DWORD size  = DWORD(buffer.size());
        if (WNetEnumResourceW(handle, &count, buffer.data(), &size) != NO_ERROR)
            break;                 // ERROR_NO_MORE_ITEMS or failure

        const auto *resources = reinterpret_cast<const NETRESOURCEW *>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            const NETRESOURCEW &resource = resources[i];
            if (!resource.lpLocalName)
                continue;

            const QString letter = QString::fromWCharArray(resource.lpLocalName).toUpper();
            if (letter.size() != 2 || letter[1] != ':')
                continue;

            // Connected letters were already listed with their live state.
            const int bit = letter[0].unicode() - 'A';
            if (bit < 0 || bit >= kDriveLetterCount || (connectedMask & (1u << bit)))
                continue;

            DriveInfo drive;
            drive.rootPath   = letter + '/';
            drive.kind       = DriveInfo::Kind::MappedDrive;
            drive.state      = DriveInfo::State::Offline;
            if (resource.lpRemoteName)
                drive.remotePath = QString::fromWCharArray(resource.lpRemoteName);
            drives.append(drive);
        }
    }

    WNetCloseEnum(handle);
}

void appendMappedDrives(QList<DriveInfo> &drives, bool probeNetwork)
{
    // Only reads the drive bitmask — no I/O.
    const DWORD connectedMask = GetLogicalDrives();

    for (int bit = 0; bit < kDriveLetterCount; ++bit) {
        if (!(connectedMask & (1u << bit)))
            continue;

        const QString letter = QString(QChar('A' + bit)) + ':';
        const std::wstring typeRoot = (letter + '\\').toStdWString();
        if (GetDriveTypeW(typeRoot.c_str()) != DRIVE_REMOTE)
            continue;

        DriveInfo drive;
        drive.rootPath   = letter + '/';
        drive.kind       = DriveInfo::Kind::MappedDrive;
        drive.remotePath = remotePathFor(letter);

        if (probeNetwork) {
            // Can hang for the network timeout on an unreachable server.
            const QStorageInfo storage(drive.rootPath);
            drive.state = storage.isReady() ? DriveInfo::State::Ready
                                            : DriveInfo::State::Offline;
            if (drive.state == DriveInfo::State::Ready)
                applyStorageInfo(drive, storage);
        }
        drives.append(drive);
    }

    appendRememberedDrives(drives, connectedMask);
}

// UNC targets of Explorer's "Add a network location" entries. Each entry is a
// folder with a target.lnk (older Windows: a plain .lnk). Reading the links is
// local disk I/O only — the server is never contacted here.
QStringList networkShortcutTargets()
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return {};

    const QDir shortcutDir(appData + QStringLiteral("/Microsoft/Windows/Network Shortcuts"));
    QStringList targets;

    for (const QFileInfo &entry : shortcutDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
        const QFileInfo link = entry.isDir()
                                   ? QFileInfo(QDir(entry.absoluteFilePath()).filePath("target.lnk"))
                                   : entry;
        // FTP / web locations resolve to something other than a UNC path — skipped.
        const QString target = QDir::fromNativeSeparators(link.symLinkTarget());
        if (target.startsWith(QLatin1String("//")))
            targets << target;
    }
    return targets;
}

void appendNetworkShares(QList<DriveInfo> &drives, const QStringList &knownShares, bool probeNetwork)
{
    for (const QString &share : networkShortcutTargets() + knownShares)
        DriveScanner::upsertNetworkShare(drives, share, DriveInfo::State::Unknown);

    if (!probeNetwork)
        return;

    // Probed after dedup so shares covered by a mapped letter are never touched.
    for (DriveInfo &drive : drives) {
        if (drive.kind == DriveInfo::Kind::NetworkShare) {
            // Can hang for the network timeout on an unreachable server.
            drive.state = QFileInfo(drive.rootPath).isDir() ? DriveInfo::State::Ready
                                                            : DriveInfo::State::Offline;
        }
    }
}
#endif

}

QList<DriveInfo> DriveScanner::scan(bool probeNetwork, const QStringList &knownShares)
{
    QList<DriveInfo> drives;

#ifdef Q_OS_WIN
    appendLocalVolumes(drives);
    // Mapped drives first — upsertNetworkShare() skips shares they already cover.
    appendMappedDrives(drives, probeNetwork);
    appendNetworkShares(drives, knownShares, probeNetwork);
#else
    Q_UNUSED(probeNetwork);
    Q_UNUSED(knownShares);
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (storage.isReady())
            drives.append(makeLocalDrive(storage));
    }
#endif

    // Volume enumeration order is arbitrary — keep drives in A..Z order,
    // shares without a letter at the end.
    std::sort(drives.begin(), drives.end(), [](const DriveInfo &a, const DriveInfo &b) {
        const bool aIsShare = a.kind == DriveInfo::Kind::NetworkShare;
        const bool bIsShare = b.kind == DriveInfo::Kind::NetworkShare;
        if (aIsShare != bIsShare)
            return bIsShare;
        return a.rootPath.compare(b.rootPath, Qt::CaseInsensitive) < 0;
    });
    return drives;
}

void DriveScanner::carryOverNetworkState(QList<DriveInfo> &fresh, const QList<DriveInfo> &previous)
{
    for (DriveInfo &drive : fresh) {
        if (!drive.isNetwork() || drive.state != DriveInfo::State::Unknown)
            continue;

        for (const DriveInfo &old : previous) {
            // Same letter mapped to a different share is a new drive — stays Unknown.
            if (old.kind == drive.kind
                && old.rootPath.compare(drive.rootPath, Qt::CaseInsensitive) == 0
                && old.remotePath.compare(drive.remotePath, Qt::CaseInsensitive) == 0) {
                drive = old;
                break;
            }
        }
    }
}

bool DriveScanner::upsertNetworkShare(QList<DriveInfo> &drives, const QString &uncPath,
                                      DriveInfo::State state)
{
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(uncPath));
    const QStringList parts = root.split('/', Qt::SkipEmptyParts);   // host, share, …
    if (!root.startsWith(QLatin1String("//")) || parts.size() < 2)
        return false;

    const QString remote = QDir::toNativeSeparators(root);

    for (DriveInfo &drive : drives) {
        if (drive.kind == DriveInfo::Kind::MappedDrive
            && drive.remotePath.compare(remote, Qt::CaseInsensitive) == 0)
            return false;

        if (drive.kind == DriveInfo::Kind::NetworkShare
            && drive.rootPath.compare(root, Qt::CaseInsensitive) == 0) {
            if (state == DriveInfo::State::Unknown || drive.state == state)
                return false;
            drive.state = state;
            return true;
        }
    }

    DriveInfo share;
    share.rootPath    = root;
    share.remotePath  = remote;
    share.displayName = parts.last();
    share.kind        = DriveInfo::Kind::NetworkShare;
    share.state       = state;
    drives.append(share);
    return true;
}
