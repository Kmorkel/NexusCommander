#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QStyledItemDelegate>
#include "fileJob.h"

/**
 * @brief Table model that owns a QVector<FileJob> — one row per job.
 *
 * All job state lives here. JobQueueWidget only reads via the model API
 * and never touches table items directly.
 */
class JobModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { ColType, ColFiles, ColProgress, ColSpeed, ColTime, ColStatus, ColCount };

    explicit JobModel(QObject *parent = nullptr);

    /// @brief Job UUID at the given model row.
    QUuid           jobIdAt(int row)     const;
    /// @brief Read-only pointer to the job at @p row, or nullptr.
    const FileJob  *jobAt(int row)       const;
    /// @brief Read-only pointer to the job with the given @p id (O(1) hash lookup), or nullptr.
    const FileJob  *jobById(QUuid id)    const;
    /// @brief Status of the job at @p row.
    FileJob::Status jobStatusAt(int row) const;
    /// @brief Number of jobs that are Queued, Running, or Paused.
    int             activeJobCount()     const;
    /// @brief Total number of jobs in the model.
    int             jobCount()           const;
    /// @brief 1-based ordinal of the currently running job (for "Job 3/5" display).
    ///        Returns -1 if no job is currently running.
    int             runningJobIndex()    const;
    /// @brief Pointer to the currently running job, or nullptr.
    const FileJob  *runningJob()         const;
    /// @brief Format milliseconds as "H:MM:SS" or "M:SS".
    static QString formatDuration(qint64 ms);
    /// @brief Remaining-time string for @p job, or empty if not enough data.
    static QString etaString(const FileJob &job);

    int      rowCount(const QModelIndex &parent = {})    const override;
    int      columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void addJob(FileJob job);
    void removeFinishedJobs();

    void setJobScanning(QUuid id);
    void setJobStarted(QUuid id, qint64 bytesTotal);
    void setJobProgress(QUuid id, qint64 bytesDone);
    void setJobPaused(QUuid id);
    void setJobResumed(QUuid id);
    void setJobFinished(QUuid id, FileJob::Status status, const QString &error);

private:
    /// @brief Resolve a job UUID to a model row index (-1 if not found).
    int  rowForJob(const QUuid &id) const;
    /// @brief Emit dataChanged for all columns of @p row.
    void emitRowChanged(int row);
    /// @brief Emit dataChanged for a column range of @p row.
    void emitRowChanged(int row, int firstCol, int lastCol);

    QVector<FileJob>    m_jobs;
    QHash<QUuid, int>   m_idToRow;
    QSet<QUuid>         m_scanning;      ///< Jobs currently in calcTotalBytes phase.
    /// Cache for runningJob() — set by setJobScanning/setJobStarted,
    /// cleared by setJobFinished. Independent of FileWorkerThread::m_runningJobId:
    /// the worker tracks execution state, this tracks the model's view of it.
    QUuid               m_runningJobId;
};

/// @brief Paints a native progress bar in the ColProgress column (no widget needed).
class ProgressBarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};
