#pragma once

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QUuid>

// 1 MiB in bytes — matches Windows Explorer's speed-display convention.
// Used *only* at the unit boundaries: UI input (MB/s → bytes/s in the setter)
// and display output (bytes/s → MB/s in JobModel::data). All internal math
// runs in bytes/s so this constant never appears in throttling or ETA code.
inline constexpr double kBytesPerMiB = 1024.0 * 1024.0;

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
        Cancelled   ///< Cancelled by the user.
    };

    /// Strategy for resolving naming conflicts at the destination.
    enum class ConflictAction {
        Ask,        ///< Prompt the user via a dialog.
        Overwrite,  ///< Replace the existing file.
        Skip,       ///< Leave the existing file untouched.
        KeepBoth    ///< Keep both — rename the new file (e.g. file(2).txt).
    };

    QUuid       id                  = QUuid::createUuid();
    Type        type                = Type::Copy;
    QStringList sources;            ///< Absolute paths of the source files/directories.
    QString     destination;        ///< Target directory (empty for Delete jobs).
    Status      status              = Status::Queued;
    int         symlinksSkipped     = 0;   ///< Number of symlinks silently skipped
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
        case Type::Copy:   return "Copy";
        case Type::Move:   return "Move";
        case Type::Delete: return "Delete";
        }
        return {};
    }

    /// @return Human-readable status name.
    QString statusString() const {
        switch (status) {
        case Status::Queued:    return "Queued";
        case Status::Running:   return "Running";
        case Status::Paused:    return "Paused";
        case Status::Done:      return "Done";
        case Status::Failed:    return "Failed";
        case Status::Cancelled: return "Cancelled";
        case Status::DoneWithWarnings: return "Done (warnings)";
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
