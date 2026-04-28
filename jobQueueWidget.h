#pragma once

#include <QDialog>
#include <QTableView>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QElapsedTimer>
#include "fileJob.h"
#include "fileWorkerThread.h"
#include "jobModel.h"

/**
 * @brief Non-modal dialog that displays all queued/running/finished file jobs.
 *
 * Shows a table with columns for type, file names, progress bar, speed (MB/s),
 * estimated remaining time, and status. Provides global controls (Pause All,
 * Resume All, Cancel All, Clear finished) and a per-job context menu.
 *
 * Uses a JobModel (QAbstractTableModel) + QTableView for clean data/view
 * separation. All job state lives in the model; this widget only connects
 * worker signals to model updates and handles UI chrome.
 */
class JobQueueWidget : public QDialog
{
    Q_OBJECT

public:
    explicit JobQueueWidget(FileWorkerThread *worker, QWidget *parent = nullptr);

    /// @brief Insert a new job into the model and show the dialog.
    void addJob(FileJob job);

    /// @brief Returns the source dirs + destination of a finished job, or empty
    /// if the job is no longer in the model. Used by MainWindow to refresh only
    /// the panels that actually point to one of these directories.
    QStringList affectedDirsForJob(QUuid jobId) const;

public slots:
    void onJobScanning(QUuid jobId);
    void onJobStarted(QUuid jobId, qint64 bytesTotal);
    void onJobProgress(QUuid jobId, qint64 bytesDone);
    void onJobPaused(QUuid jobId);
    void onJobResumed(QUuid jobId);
    void onJobFinished(QUuid jobId, FileJob::Status status, const QString &error);
    void onAllJobsDone();

    /**
     * @brief Show a conflict-resolution dialog and unblock the worker.
     *
     * The worker thread is blocked on a QWaitCondition until this slot
     * calls FileWorkerThread::resolveConflict().
     */
    void onConflictDetected(QUuid jobId, const QString &fileName,
                            const QString &destPath);

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
    QTableView       *m_view        = nullptr;
    QCheckBox        *m_shutdownBox = nullptr;
    QDoubleSpinBox   *m_speedSpin   = nullptr;
    QPushButton      *m_pauseAllBtn = nullptr;
    QPushButton      *m_resumeAllBtn = nullptr;
    QPushButton      *m_cancelAllBtn = nullptr;
    QPushButton      *m_clearBtn    = nullptr;
    QLabel           *m_statusLabel = nullptr;
    QLabel           *m_pauseBanner = nullptr;

    // Throttles updateOverallStatus() to ~1 Hz. Progress ticks fire at 20 Hz
    // and would otherwise rebuild the status label up to 20x/s for no visible
    // benefit (ETA only changes meaningfully on a per-second cadence).
    QElapsedTimer     m_statusThrottle;

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
};
