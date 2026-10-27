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
 *      │                                    │   │   │   └─ speed limiting (timed wait)
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
 * - Speed-limit sleep waits on @c m_pauseCondition with a timeout, which
 *   releases @c m_mutex — UI-thread calls (pause, resume, setSpeedLimit)
 *   never block and wake the sleep immediately.
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

    /// @brief Unblock the worker after the read-only dialog (see readOnlyDetected()).
    /// @p action: Proceed, CopyOnly (move only) or Skip; "Cancel Job" = cancelJob() + Skip.
    void resolveReadOnly(FileJob::ReadOnlyAction action, bool applyToAll = false);

signals:
    // Signal parameters: small types (QUuid, int, double) are passed by value
    // intentionally — these signals cross thread boundaries via QueuedConnection,
    // where Qt copies arguments anyway. Only QString / QList use const& (COW semantics).

    void jobScanning(QUuid id);                   ///< Emitted before calcTotalBytes — UI can show "Scanning...".
    /// @brief Emitted when a job begins processing, after the size scan.
    /// @p sourceBytes holds the size of every source (Delete jobs: 1 each).
    void jobStarted(QUuid id, qint64 bytesTotal, const QList<qint64> &sourceBytes);
    void jobProgress(QUuid id, qint64 bytesDone); ///< Emitted per chunk during copy/move.
    void jobPaused(QUuid id);                   ///< Emitted once when the worker enters pause.
    void jobResumed(QUuid id);                  ///< Emitted once when the worker leaves pause.
    /// @brief Emitted when a job completes (any status). @p unchanged: nothing was
    /// written, moved or deleted (cancelled before start, or every item skipped)
    /// and no link was skipped.
    void jobFinished(QUuid id, FileJob::Status status, const QString &error, bool unchanged);
    /// @brief Source @p index of the job (a selected file or folder) is being processed.
    /// @p bytesTotal is its size — for Delete jobs 1 (progress counts items there).
    void sourceStarted(QUuid id, int index, qint64 bytesTotal);
    /// @brief Source @p index is finished: Done, DoneWithWarnings (files inside were
    /// skipped), Skipped, Failed or Cancelled. Sources never started get no signal.
    void sourceFinished(QUuid id, int index, FileJob::Status status);
    void allJobsDone();                         ///< Emitted when the queue is fully drained.

    /// @brief The destination cannot hold the job — it fails before anything is written.
    void insufficientSpace(QUuid jobId, const QString &destination, qint64 bytesNeeded,
                           qint64 bytesAvailable);

    /**
     * @brief A destination file already exists — UI should show a conflict dialog.
     *
     * The worker thread is blocked until resolveConflict() is called.
     * @p readOnly: the existing file is read-only — the dialog should say so.
     */
    void conflictDetected(QUuid jobId, const QString &fileName,
                          const QString &destPath, bool readOnly);

    /**
     * @brief A read-only file is about to be deleted (Delete job), or a source that
     * is / contains read-only files is about to be moved (@p isMove).
     *
     * The worker thread is blocked until resolveReadOnly() is called.
     */
    void readOnlyDetected(QUuid jobId, const QString &path, bool isMove);

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
    bool             m_ioCancellable  = false; ///< Worker is inside a read/write that cancel may abort.
    void            *m_threadHandle   = nullptr; ///< Windows: worker thread HANDLE for CancelSynchronousIo().

    // Conflict resolution — uses a separate mutex to avoid deadlock with pause.
    QMutex                  m_conflictMutex;
    QWaitCondition          m_conflictCondition;
    FileJob::ConflictAction m_conflictResult     = FileJob::ConflictAction::Skip;
    bool                    m_conflictPending    = false;
    bool                    m_conflictApplyToAll = false;
    // Read-only question — same mutex / condition, only one dialog is open at a time.
    FileJob::ReadOnlyAction m_readOnlyResult     = FileJob::ReadOnlyAction::Skip;
    bool                    m_readOnlyPending    = false;
    bool                    m_readOnlyApplyToAll = false;

    /// @brief Dispatch a single job to runCopyJob/runMoveJob/runDeleteJob.
    void processJob(FileJob &job);
    /// @brief Determine final status and emit jobFinished.
    void finalizeJob(FileJob &job);
    /// @brief Check the destination's free space against the scanned job size.
    /// On failure sets errorMessage and emits insufficientSpace().
    bool hasEnoughSpace(FileJob &job);
    /// @brief Source skipped on conflict: count its bytes as processed, report it Skipped.
    void skipSource(FileJob &job, int index, qint64 sourceBytes, qint64 &bytesDone);
    /// @brief Copy / Move never touch a link source or a left-out .part file —
    /// report it Skipped and return true. Links count as a warning.
    bool skipLinkOrPartFile(FileJob &job, int index, const QFileInfo &sourceInfo);
    /// @brief Destination of a Copy / Move source: an existing folder of the same
    /// name is merged into without asking, anything else goes through
    /// resolveDestinationConflict(). Empty = skip.
    QString destinationFor(const QFileInfo &sourceInfo, FileJob &job,
                           FileJob::ConflictAction &conflictAction);
    /// @brief Emit sourceFinished() for a processed source. @p skippedBefore is
    /// job.itemsSkipped before it started — more now means files inside were skipped.
    void reportSourceResult(FileJob &job, int index, bool success, int skippedBefore);
    /// @brief Execute a copy job (all sources).
    void runCopyJob(FileJob &job);
    /// @brief Execute a move job (all sources).
    void runMoveJob(FileJob &job);
    /// @brief Execute a delete job (all sources).
    void runDeleteJob(FileJob &job);
    /// @brief Move one file/directory: try rename first, fall back to copy+delete.
    /// A folder onto an existing folder is merged (mergeMoveDirectory()).
    /// @p srcBytes advances the progress on a rename (nothing is copied then).
    bool moveSource(const QString &src, const QString &dst, const QFileInfo &srcInfo,
                    qint64 srcBytes, FileJob &job,
                    qint64 &bytesDone, FileJob::ConflictAction &conflictAction);
    /// @brief Move the contents of @p src into the existing folder @p dst, entry by
    /// entry (conflicts asked per file), then remove @p src if it is empty.
    bool mergeMoveDirectory(const QString &src, const QString &dst, FileJob &job,
                            qint64 &bytesDone, FileJob::ConflictAction &conflictAction);
    /// @brief Ask the UI what to do with the read-only @p path (blocks until
    /// resolveReadOnly()); "Apply to all" is kept in job.readOnlyAction.
    FileJob::ReadOnlyAction askReadOnly(FileJob &job, const QString &path, bool isMove);

    /// @brief Copy a single file in chunks with progress, pause, and speed limiting.
    bool copyFile(const QString &src, const QString &dst,
                  FileJob &job, qint64 &bytesDone);
#ifdef Q_OS_WIN
    /// @brief copyFile() without a speed limit: CopyFileExW (Windows' copy engine,
    /// several requests in flight on SMB). Pause, cancel and a limit set mid-file
    /// run in its progress callback. @p copiedBytes: content bytes copied.
    bool copyFileNative(const QString &src, const QString &dst,
                        FileJob &job, qint64 &bytesDone, qint64 &copiedBytes);
#endif
    /// @brief Recursively copy a directory tree.
    bool copyRecursive(const QString &src, const QString &dst,
                       FileJob &job, qint64 &bytesDone, FileJob::ConflictAction &conflictAction);

    /// @brief Cross-device move fallback: copy source, then delete original.
    bool moveByCopy(const QString &src, const QString &dst,
                         FileJob &job, qint64 &bytesDone, FileJob::ConflictAction &conflictAction);

    /// @brief Cancellable recursive delete of a file or folder — replaces
    /// QDir::removeRecursively so the user can abort mid-operation on large trees.
    /// @p confirmReadOnly: ask per read-only file (Delete jobs); otherwise they are
    /// deleted without asking (source of a move the user already agreed to).
    bool removeRecursive(const QString &path, FileJob &job, bool confirmReadOnly);

    /// @brief Copy mtime and permissions from src to dst. Best-effort, failures are ignored.
    void copyMetadata(const QString &src, const QString &dst) const;

    QString resolveDestinationConflict(const QString &dst, const QUuid &jobId,
                            FileJob::ConflictAction &defaultAction);
    QString makeUniquePath(const QString &dst) const;

    /// @brief Block the worker thread while the job is paused.
    void waitIfPaused(const QUuid &id);
    /// @brief Sleep if throughput exceeds the configured speed limit.
    void   applySpeedThrottle(SpeedTracker &tracker);
    /// @brief Timed wait on m_pauseCondition — woken early by pause/cancel/stop.
    void   sleepInterruptible(qint64 totalMs);
    /// @brief True if the current job should break out of a sleep cycle.
    bool   shouldAbortSleep() const;
    /// @brief Return a chunk size adapted to the speed limit, or without one to
    /// @p measuredBps (speed of the current file so far, 0 = not measured yet).
    qint64 adaptiveChunkSize(double measuredBps) const;
    /// @brief Sum up byte sizes for all source paths (pausable/cancellable).
    /// If @p perSourceBytes is non-null, also records each source's individual
    /// byte size — used by Move jobs so moveSource() can bump progress on a
    /// same-volume rename without a second recursive walk.
    /// If @p readOnlySources is non-null, sources that are or contain read-only
    /// files are added to it (same walk, no extra I/O).
    /// If @p partFileDirs is non-null, folders holding a left-out .part file
    /// (job.skipPartFiles) somewhere below are added to it.
    qint64 calculateTotalBytes(const FileJob &job,
                               QHash<QString, qint64> *perSourceBytes = nullptr,
                               QSet<QString> *readOnlySources = nullptr,
                               QSet<QString> *partFileDirs = nullptr);
    /// @brief Recursively sum file sizes in a directory (hidden / system files
    /// included, like the copy; .part files left out if the job skips them).
    /// Sets *@p containsReadOnly if a read-only file is found.
    qint64 calculateDirectorySize(const QString &path, const FileJob &job,
                                  bool *containsReadOnly = nullptr,
                                  QSet<QString> *partFileDirs = nullptr);
    /// @brief Thread-safe check whether a job has been cancelled.
    bool   isCancelled(const QUuid &id) const;
    /// @brief Block while the job is paused, then check for cancel — the check
    /// every loop runs before its next item.
    bool   isCancelledAfterPause(const QUuid &id);

    /// @brief Mark the start of a read/write in copyFile() that cancel may abort
    /// (a dead NAS blocks it until the SMB timeout). False if already cancelled.
    bool   beginCancellableIo(const QUuid &id);
    /// @brief End of the section started by beginCancellableIo().
    void   endCancellableIo();
    /// @brief Abort the worker's blocked read/write. @c m_mutex must be held.
    void   cancelPendingIo();
};
