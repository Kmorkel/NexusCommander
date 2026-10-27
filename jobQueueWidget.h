#pragma once

#include <QDialog>
#include <QTreeView>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QFuture>
#include <QHash>
#include <QTimer>
#include "fileJob.h"
#include "fileWorkerThread.h"
#include "jobModel.h"
#include "taskbarProgress.h"

/**
 * @brief Non-modal dialog that displays all queued/running/finished file jobs.
 *
 * Shows a table with columns for type, file names, progress bar, speed (MB/s),
 * estimated remaining time, and status. Provides global controls (Pause All,
 * Resume All, Cancel All, Clear finished) and a per-job context menu.
 *
 * Uses a JobModel (tree model) + QTreeView for clean data/view separation.
 * A job with several files expands on double-click into one row per file
 * (indented under the job in the Files column) with its own progress and
 * status. All job state lives in the model; this widget only connects
 * worker signals to model updates and handles UI chrome.
 */
class JobQueueWidget : public QDialog
{
    Q_OBJECT

public:
    explicit JobQueueWidget(FileWorkerThread *worker, QWidget *parent = nullptr);
    ~JobQueueWidget() override;

    /// @brief Insert a new job into the model. The dialog is not shown — the
    /// main window's status-bar button signals the job (jobAdded()).
    void addJob(FileJob job);

    /// @brief Show the dialog in front — also restores it when it is minimized.
    void showRestored();

    /// @brief Pause / resume the whole queue, like Pause All / Resume All.
    void setQueuePaused(bool paused);

    /// @brief Returns the source dirs + destination of a finished job, or empty
    /// if the job is no longer in the model. Used by MainWindow to refresh only
    /// the panels that actually point to one of these directories.
    QStringList affectedDirsForJob(QUuid jobId) const;

    /// @brief Number of jobs that are queued, running, or paused.
    int activeJobCount() const;

public slots:
    void onJobScanning(QUuid jobId);
    void onJobStarted(QUuid jobId, qint64 bytesTotal, const QList<qint64> &sourceBytes);
    void onJobProgress(QUuid jobId, qint64 bytesDone);
    void onJobPaused(QUuid jobId);
    void onJobResumed(QUuid jobId);
    /// @brief A job that changed nothing (@p unchanged — cancelled before it
    /// started, or every item skipped) is removed from the list; failures stay.
    void onJobFinished(QUuid jobId, FileJob::Status status, const QString &error, bool unchanged);
    void onAllJobsDone();

    /**
     * @brief Show a conflict-resolution dialog and unblock the worker.
     *
     * The worker thread is blocked on a QWaitCondition until this slot
     * calls FileWorkerThread::resolveConflict().
     */
    void onConflictDetected(QUuid jobId, const QString &fileName,
                            const QString &destPath, bool readOnly);

    /// @brief Ask what to do with a read-only file (delete) or source (move) and
    /// unblock the worker via FileWorkerThread::resolveReadOnly().
    void onReadOnlyDetected(QUuid jobId, const QString &path, bool isMove);

    /// @brief Tell the user a copy/move was refused for lack of space at the destination.
    void onInsufficientSpace(QUuid jobId, const QString &destination,
                             qint64 bytesNeeded, qint64 bytesAvailable);

signals:
    /// @brief Progress of the current batch of jobs for the taskbar button.
    void taskbarProgressChanged(TaskbarProgress::State state, double fraction);

    /// @brief Same batch progress for the main window's status bar — shown also
    /// while this dialog is closed. @p activeJobs 0 = idle; @p remaining is the
    /// time left for all jobs ("≥ …" while sizes are unknown, empty without a speed).
    void activityChanged(int activeJobs, TaskbarProgress::State state, double fraction,
                         const QString &remaining);

    /// @brief A job was queued — the main window highlights its status-bar button.
    void jobAdded();

    /// @brief The queue went idle. @p doneJobs: finished jobs that changed something;
    /// @p failed: a job of the batch failed. Emitted before the idle activityChanged().
    void batchFinished(int doneJobs, bool failed);

    /// @brief The global pause flag changed (Pause All / Resume All / Cancel All).
    void pauseChanged(bool paused);

private slots:
    /// @brief Show per-job context menu (Pause/Resume/Cancel/Details).
    void onContextMenuRequested(const QPoint &pos);
    /// @brief Remove all finished/cancelled/failed jobs from the model.
    void onClearFinished();
    /// @brief Pause all running and queued jobs globally.
    void onPauseAll();
    /// @brief Resume all paused jobs and clear the global pause flag.
    void onResumeAll();
    /// @brief Cancel all running and queued jobs after confirmation.
    void onCancelAll();
    /// @brief Forward the spin-box value to the worker's speed limiter.
    void onSpeedLimitChanged(double value);

private:
    FileWorkerThread *m_worker      = nullptr;
    JobModel         *m_model       = nullptr;
    QTreeView        *m_view        = nullptr;
    QCheckBox        *m_shutdownBox = nullptr;
    QDoubleSpinBox   *m_speedSpin   = nullptr;
    QPushButton      *m_pauseAllBtn = nullptr;
    QPushButton      *m_resumeAllBtn = nullptr;
    QPushButton      *m_cancelAllBtn = nullptr;
    QPushButton      *m_clearBtn    = nullptr;
    QLabel           *m_statusLabel = nullptr;
    QLabel           *m_pauseBanner = nullptr;

    // Refreshes speed, remaining times and the status label once per second
    // while jobs are active — also when no progress arrives (stalled network).
    // Progress ticks fire at up to 20 Hz; the label only needs 1 Hz.
    QTimer           *m_tickTimer   = nullptr;

    // Size scans of queued copy / move jobs for the remaining-time estimate —
    // the worker scans a job only when it starts it.
    QHash<QUuid, QFuture<QList<qint64>>> m_estimates;

    // Jobs added since the queue was last idle — the taskbar shows their progress.
    int               m_batchTotal    = 0;
    int               m_batchFinished = 0;
    int               m_batchDone     = 0;      ///< Finished jobs that changed something.
    bool              m_batchFailed   = false;

    void setupUi();
    void connectSignals();
    /// @brief Append Pause/Resume/Cancel actions for @p jobId based on its current status.
    void addJobControlActions(QMenu &menu, FileJob::Status status, QUuid jobId);
    /// @brief Set window title to "Job Queue" or "Job Queue — N active".
    void updateTitle();
    /// @brief Refresh the status label with current job index and ETA.
    void updateOverallStatus();
    /// @brief Open a detail dialog showing source/destination paths for @p row.
    void showJobDetails(int row);
    /// @brief Update the pause banner and button labels to match global-pause state.
    void updatePauseUi();
    /// @brief Calculate remaining-time string for the currently running job.
    QString calculateEtaString() const;
    /// @brief Scan the sources of a queued copy / move job in the background.
    void startEstimate(const FileJob &job);
    /// @brief Stop the estimate scan of @p jobId — the worker has its own scan now.
    void cancelEstimate(QUuid jobId);
    /// @brief Emit taskbarProgressChanged() and activityChanged() for the current batch.
    void updateTaskbar();
};
