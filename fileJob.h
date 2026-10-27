#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

// 1 MiB in bytes — matches Windows Explorer's speed-display convention.
// Used *only* at the unit boundaries: UI input (MB/s → bytes/s in the setter)
// and display output (bytes/s → MB/s in JobModel::data). All internal math
// runs in bytes/s so this constant never appears in throttling or ETA code.
inline constexpr double kBytesPerMiB = 1024.0 * 1024.0;

/// @return True for an unfinished download ("*.part") — hidden in the panels and
/// left out of copy / move while View → Hide .part Files is on.
inline bool isPartFileName(const QString &fileName)
{
    return fileName.endsWith(QLatin1String(".part"), Qt::CaseInsensitive);
}

/**
 * @brief Plain data struct describing a single queued file operation.
 *
 * Each FileJob carries a unique ID, a list of source paths, an optional
 * destination, and runtime bookkeeping (progress, speed, error message).
 * Jobs are created on the UI thread and processed sequentially by FileWorkerThread.
 */
struct FileJob
{
    /// Operation type.
    enum class Type   { Copy, Move, Delete };

    /// Lifecycle status — transitions are managed by FileWorkerThread.
    enum class Status {
        Queued,     ///< Waiting in the queue.
        Running,    ///< Currently being processed by the worker thread.
        Paused,     ///< Paused — resumes when resumeJob() is called.
        Done,       ///< Finished successfully.
        DoneWithWarnings, ///< Used for skipped symlinks or other non-fatal warnings.
        Failed,     ///< An error occurred (see @c errorMessage).
        Cancelled,  ///< Cancelled by the user.
        Skipped     ///< Single source only (never a whole job): skipped on conflict or a link.
    };

    /// Strategy for resolving naming conflicts at the destination.
    enum class ConflictAction {
        Ask,        ///< Prompt the user via a dialog.
        Overwrite,  ///< Replace the existing file.
        Skip,       ///< Leave the existing file untouched.
        KeepBoth    ///< Keep both — rename the new file (e.g. file(2).txt).
    };

    /// Answer to "this is read-only" — asked before deleting a read-only file
    /// or moving a source that is / contains read-only files.
    enum class ReadOnlyAction {
        Ask,        ///< Prompt the user via a dialog.
        Proceed,    ///< Delete / move it anyway (the read-only flag is cleared where needed).
        CopyOnly,   ///< Move only: copy it, leave the original where it is.
        Skip        ///< Leave it untouched.
    };

    QUuid       id                  = QUuid::createUuid();
    Type        type                = Type::Copy;
    QStringList sources;            ///< Absolute paths of the source files/directories.
    QString     destination;        ///< Target directory (empty for Delete jobs).
    Status      status              = Status::Queued;
    int         symlinksSkipped     = 0;   ///< Number of symlinks / junctions silently skipped
    int         itemsSkipped        = 0;   ///< Skipped on a destination conflict or kept as read-only
    int         readOnlyCopied      = 0;   ///< Move: read-only sources copied instead of moved
    int         itemsChanged        = 0;   ///< Files / folders written, moved or deleted — 0 = nothing changed
    // Move: sources that are / contain read-only files (found by the size scan).
    QSet<QString> readOnlySources;
    ReadOnlyAction readOnlyAction   = ReadOnlyAction::Ask;  ///< "Apply to all" of the read-only dialog
    bool        skipPartFiles       = false;  ///< Copy / Move: leave ".part" files out (set when enqueued)
    // Move with skipPartFiles: folders with a .part file somewhere below (found by
    // the size scan) — moved entry by entry so the .part files stay behind.
    QSet<QString> partFileDirs;
    qint64      bytesTotal          = 0;
    qint64      bytesDone           = 0;
    // Per-source byte sizes, populated by calculateTotalBytes() during the
    // pre-scan and read by moveSource() to bump progress on a same-volume
    // rename without a second recursive walk.
    QHash<QString, qint64> bytesPerSource;
    double      bytesPerSecond      = 0.0;   ///< 0 = unknown/paused. Converted to MiB/s at display time.
    QElapsedTimer speedTimer;            ///< Monotonic baseline for the speed sample window.
    qint64      lastBytesDone       = 0;   ///< bytesDone snapshot at last speed update.
    QString     errorMessage;           ///< Non-empty on hard failure.
    QString     warningMessage;         ///< Non-empty on non-fatal warnings (e.g. skipped symlinks).

    /// @return Human-readable type name ("Copy", "Move", "Delete").
    QString typeString() const {
        switch (type) {
        case Type::Copy:   return QCoreApplication::translate("FileJob", "Copy");
        case Type::Move:   return QCoreApplication::translate("FileJob", "Move");
        case Type::Delete: return QCoreApplication::translate("FileJob", "Delete");
        }
        return {};
    }

    /// @return Human-readable status name.
    QString statusString() const { return statusText(status); }

    /// @return Human-readable name of @p status — also used for the per-source rows.
    static QString statusText(Status status) {
        switch (status) {
        case Status::Queued:           return QCoreApplication::translate("FileJob", "Queued");
        case Status::Running:          return QCoreApplication::translate("FileJob", "Running");
        case Status::Paused:           return QCoreApplication::translate("FileJob", "Paused");
        case Status::Done:             return QCoreApplication::translate("FileJob", "Done");
        case Status::Failed:           return QCoreApplication::translate("FileJob", "Failed");
        case Status::Cancelled:        return QCoreApplication::translate("FileJob", "Cancelled");
        case Status::DoneWithWarnings: return QCoreApplication::translate("FileJob", "Done (warnings)");
        case Status::Skipped:          return QCoreApplication::translate("FileJob", "Skipped");
        }
        return {};
    }

    /// @return Short display label, for example "Copy  readme.txt (+3)".
    QString label() const {
        if (sources.isEmpty())
            return typeString();
        const QString firstName = QFileInfo(sources.first()).fileName();

        return sources.size() > 1
            ? QString("%1  %2 (+%3)").arg(typeString(), firstName).arg(sources.size() - 1)
            : QString("%1  %2").arg(typeString(), firstName);
    }
};

// Required so Qt can copy these types across thread boundaries in QueuedConnections.
Q_DECLARE_METATYPE(FileJob::Status)
Q_DECLARE_METATYPE(FileJob::ConflictAction)
Q_DECLARE_METATYPE(FileJob::ReadOnlyAction)
