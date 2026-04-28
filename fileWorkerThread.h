#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QSet>
#include <QElapsedTimer>
#include "fileJob.h"

/// @brief Speed sampling window in milliseconds.
/// Bytes-this-second counter is reset every @c kSpeedSampleWindowMs ticks.
inline constexpr int kSpeedSampleWindowMs = 1000;

/// @brief Speed tracking + throttling state for copyFile().
struct SpeedTracker {
    QElapsedTimer timer;
    qint64 bytesThisSecond = 0;

    void start()                { timer.start(); }
    void addBytes(qint64 bytes) { bytesThisSecond += bytes; }

    /// @brief Reset the byte counter once the 1 s sample window has elapsed.
    void resetIfWindowExpired() {
        if (timer.elapsed() >= kSpeedSampleWindowMs) {
            bytesThisSecond = 0;
            timer.restart();
        }
    }
};

/**
 * @brief Background worker thread that processes file operations sequentially.
 *
 * FileWorkerThread owns a job queue and a persistent thread. Jobs (Copy, Move, Delete)
 * are enqueued from the UI thread via enqueue() and executed one at a time in
 * FIFO order. The worker supports per-job pause/resume, cancellation, speed
 * limiting, and interactive conflict resolution.
 *
 * @par Threading model
 * @code
 *  UI Thread                              Worker Thread
 *  ─────────                              ─────────────
 *  enqueue(job)                           run()
 *      │                                    │
 *      ├─► m_queue.enqueue() ──────────────►│  (wakes via m_condition)
 *      │                                    ├─ processJob()
 *      │                                    │   ├─ runCopyJob() / runMoveJob() / runDeleteJob()
 *      │                                    │   │   ├─ copyFile() [1 MB chunks]
 *      │                                    │   │   │   ├─ waitIfPaused()
 *      │                                    │   │   │   │   └─ blocks on m_pauseCondition
 *      │   pauseJob() / resumeJob() ───────►│   │   │   │       (until resumed/cancelled)
 *      │                                    │   │   │   └─ speed limiting (msleep)
 *      │                                    │   │   └─ resolveDestinationConflict()
 *      │                                    │   │       ├─ emit conflictDetected() ──►┐
 *      │   ┌◄── show conflict dialog ◄──────┤   │       └─ block on m_conflictCondition│
 *      │   └──► resolveConflict() ──────────►   │           (until UI responds)       │
 *      │                                    │   └─ finalizeJob()
 *      │◄── jobStarted / jobProgress ◄──────┤       emit jobFinished()
 *      │◄── jobFinished ◄──────────────────┤
 *      │                                    └─ loop (next job or wait)
 * @endcode
 *
 * @par Key synchronisation rules
 * - @c m_mutex guards the job queue, pause/cancel sets, and speed limit.
 * - @c m_conflictMutex guards conflict resolution state (separate to avoid
 *   deadlock with pause operations).
 * - Speed-limit sleep releases @c m_mutex @b before calling msleep(),
 *   so UI-thread calls (pause, resume, setSpeedLimit) never block.
 * - Connections from worker signals to UI slots use Qt::QueuedConnection
 *   (automatic for cross-thread connects).
 */
class FileWorkerThread : public QThread
{
    Q_OBJECT

public:
    explicit FileWorkerThread(QObject *parent = nullptr);
    ~FileWorkerThread() override;

    /// @brief Add a job to the queue. Thread-safe; wakes the worker if idle.
    void   enqueue(const FileJob &job);

    /// @brief Mark a single job for cancellation (skipped if still queued).
    void   cancelJob(const QUuid &id);
    /// @brief Cancel all queued and the currently running job.
    void   cancelAll();

    /// @brief Pause a running job. The worker blocks in waitIfPaused().
    void   pauseJob(const QUuid &id);
    /// @brief Resume a previously paused job.
    void   resumeJob(const QUuid &id);
    /// @brief Pause every queued and currently running job, and future jobs.
    void   pauseAll();
    /// @brief Resume all paused jobs and allow future jobs to run.
    void   resumeAll();
    /// @brief Returns true if global pause is active (new jobs start paused).
    bool   isGlobalPaused() const;

    /// @brief Set a global speed cap (MB/s). 0 = unlimited.
    void   setSpeedLimitMBs(double mbps);

    /// @brief Source paths of all queued + currently running jobs.
    ///
    /// Used by the delete handler to warn about files that are in use.
    QStringList activeSourcePaths() const;

    /**
     * @brief Unblock the worker after a conflict dialog.
     * @param action    The user's choice (Overwrite / Skip / KeepBoth).
     * @param applyToAll If true, apply this action to all remaining conflicts in the job.
     *
     * Called from the UI thread after the conflict dialog is closed.
     * Wakes the worker thread which is blocked on m_conflictCondition.
     */
    void resolveConflict(FileJob::ConflictAction action, bool applyToAll = false);

signals:
    // Signal parameters: small types (QUuid, int, double) are passed by value
    // intentionally — these signals cross thread boundaries via QueuedConnection,
    // where Qt copies arguments anyway. Only QString uses const& (COW semantics).

    void jobScanning(QUuid id);                   ///< Emitted before calcTotalBytes — UI can show "Scanning...".
    void jobStarted(QUuid id, qint64 bytesTotal); ///< Emitted when a job begins processing.
    void jobProgress(QUuid id, qint64 bytesDone); ///< Emitted per chunk during copy/move.
    void jobPaused(QUuid id);                   ///< Emitted once when the worker enters pause.
    void jobResumed(QUuid id);                  ///< Emitted once when the worker leaves pause.
    void jobFinished(QUuid id, FileJob::Status status, const QString &error);     ///< Emitted when a job completes (any status).
    void allJobsDone();                         ///< Emitted when the queue is fully drained.

    /**
     * @brief A destination file already exists — UI should show a conflict dialog.
     *
     * The worker thread is blocked until resolveConflict() is called.
     */
    void conflictDetected(QUuid jobId, const QString &fileName,
                          const QString &destPath);

protected:
    void run() override;

private:
    mutable QMutex   m_mutex;           ///< Mutable so that const methods can lock it for thread-safe reads.
    QWaitCondition   m_condition;       ///< Wakes the thread when a new job arrives.
    QWaitCondition   m_pauseCondition;  ///< Wakes the thread when a job is resumed.
    QQueue<FileJob>  m_queue;
    QSet<QUuid>      m_cancelled;
    QSet<QUuid>      m_paused;
    QUuid            m_runningJobId;        ///< ID of the job currently being processed (cleared between jobs).
    QStringList      m_runningJobSources;   ///< Source paths of the currently-running job. Combined with queued
                                            ///< jobs' sources to form activeSourcePaths(); needed because the
                                            ///< running job is no longer in m_queue while it executes.

    double           m_speedLimitBps  = 0.0;  ///< Stored in bytes/s. Setter converts from MB/s input.
    bool             m_stop           = false; ///< Set in destructor; checked in run() and copy loops.
    bool             m_globalPause    = false; ///< When true, newly dequeued jobs start paused.

    // Conflict resolution — uses a separate mutex to avoid deadlock with pause.
    QMutex                  m_conflictMutex;
    QWaitCondition          m_conflictCondition;
    FileJob::ConflictAction m_conflictResult     = FileJob::ConflictAction::Skip;
    bool                    m_conflictPending    = false;
    bool                    m_conflictApplyToAll = false;

    /// @brief Dispatch a single job to runCopyJob/runMoveJob/runDeleteJob.
    void processJob(FileJob &job);
    /// @brief Determine final status and emit jobFinished.
    void finalizeJob(FileJob &job);
    /// @brief Execute a copy job (all sources).
    void runCopyJob(FileJob &job);
    /// @brief Execute a move job (all sources).
    void runMoveJob(FileJob &job);
    /// @brief Execute a delete job (all sources).
    void runDeleteJob(FileJob &job);
    /// @brief Move one file/directory: try rename first, fall back to copy+delete.
    bool moveSource(const QString &src, const QString &dst, const QFileInfo &srcInfo,
                        FileJob &job,
                        qint64 &bytesDone, FileJob::ConflictAction &conflictAction);

    /// @brief Copy a single file in chunks with progress, pause, and speed limiting.
    bool copyFile(const QString &src, const QString &dst,
                  FileJob &job, qint64 &bytesDone);
    /// @brief Recursively copy a directory tree.
    bool copyRecursive(const QString &src, const QString &dst,
                       FileJob &job, qint64 &bytesDone, FileJob::ConflictAction &conflictAction);

    /// @brief Cross-device move fallback: copy source, then delete original.
    bool moveByCopy(const QString &src, const QString &dst,
                         FileJob &job, qint64 &bytesDone, FileJob::ConflictAction &conflictAction);

    /// @brief Cancellable recursive delete — replaces QDir::removeRecursively so the user
    /// can abort mid-operation on large trees.
    bool removeRecursive(const QString &path, const FileJob &job);

    /// @brief Copy mtime and permissions from src to dst. Best-effort, failures are ignored.
    void copyMetadata(const QString &src, const QString &dst) const;

    QString resolveDestinationConflict(const QString &dst, const QUuid &jobId,
                            FileJob::ConflictAction &defaultAction);
    QString makeUniquePath(const QString &dst) const;

    /// @brief Block the worker thread while the job is paused.
    void waitIfPaused(const QUuid &id);
    /// @brief Sleep if throughput exceeds the configured speed limit.
    void   applySpeedThrottle(SpeedTracker &tracker);
    /// @brief Sleep in small slices so cancel/pause remain responsive.
    void   sleepInterruptible(qint64 totalMs);
    /// @brief True if the current job should break out of a sleep cycle.
    bool   shouldAbortSleep() const;
    /// @brief Return a chunk size adapted to the current speed limit.
    qint64 adaptiveChunkSize() const;
    /// @brief Sum up byte sizes for all source paths (pausable/cancellable).
    /// If @p perSourceBytes is non-null, also records each source's individual
    /// byte size — used by Move jobs so moveSource() can bump progress on a
    /// same-volume rename without a second recursive walk.
    qint64 calculateTotalBytes(const QStringList &paths, const QUuid &jobId,
                               QHash<QString, qint64> *perSourceBytes = nullptr);
    /// @brief Recursively sum file sizes in a directory.
    qint64 calculateDirectorySize(const QString &path, const QUuid &jobId);
    /// @brief Thread-safe check whether a job has been cancelled.
    bool   isCancelled(const QUuid &id) const;
};
