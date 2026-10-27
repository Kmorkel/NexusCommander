#pragma once

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Plain snapshot of one drive for the drive dropdown.
 *
 * Built on the scan thread by DriveScanner::scan() and handed to the UI by
 * value — FilePanel never touches the filesystem to fill its dropdown, so an
 * unreachable network drive can never freeze the GUI thread.
 */
struct DriveInfo
{
    /// What the entry points at.
    enum class Kind {
        Volume,         ///< Local volume (HDD, SSD, USB) or folder mount point.
        MappedDrive,    ///< Network share with a drive letter (e.g. "Z:").
        NetworkShare    ///< Network share without a letter (e.g. "\\raspberrypi\pi-ssd").
    };

    /// Reachability. Network entries are only probed on demand (dropdown opened).
    enum class State {
        Ready,      ///< Mounted and readable.
        Offline,    ///< Network entry whose server is unreachable or not connected.
        Unknown     ///< Network entry that has not been probed yet.
    };

    QString rootPath;               ///< e.g. "C:/", "Z:/", "//raspberrypi/pi-ssd".
    QString volumeName;             ///< Volume label, empty if none / not probed.
    QString displayName;            ///< Fallback label for mount points; share name for NetworkShare.
    QString fileSystemType;
    QString remotePath;             ///< UNC target of a network entry (e.g. "\\nas\media").
    qint64  bytesTotal     = 0;
    qint64  bytesAvailable = 0;
    Kind    kind           = Kind::Volume;
    State   state          = State::Unknown;

    bool isNetwork() const { return kind != Kind::Volume; }

    /// @return Identity + visible state — used to skip UI rebuilds when nothing changed.
    QString signature() const {
        return rootPath + '|' + volumeName + '|' + remotePath + '|'
               + QString::number(int(state));
    }
};

namespace DriveScanner {

/// @brief Enumerate local volumes, mapped network drives and network shares. Blocking — run off the GUI thread.
///
/// Network shares come from Windows' "Network Shortcuts" folder (Explorer's
/// "Add a network location") plus @p knownShares (found by NetworkDiscovery
/// this session). Local volumes are always fully read; network entries are
/// only listed unless @p probeNetwork is true — probing is the one step that
/// can hang for the network timeout on an unreachable server.
QList<DriveInfo> scan(bool probeNetwork, const QStringList &knownShares);

/// @brief Copy the last known state of unprobed network entries from @p previous.
///
/// A scan without probing returns network entries as Unknown; this keeps the
/// result of the last dropdown probe until the user opens the dropdown again.
void carryOverNetworkState(QList<DriveInfo> &fresh, const QList<DriveInfo> &previous);

/// @brief Add the share @p uncPath to @p drives, or update the state of an existing entry.
///
/// Skipped when a mapped drive letter already points at the same share — the
/// letter is what the user chose. @return true if @p drives changed.
bool upsertNetworkShare(QList<DriveInfo> &drives, const QString &uncPath, DriveInfo::State state);

}
