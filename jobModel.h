#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QSet>
#include <QStyledItemDelegate>
#include "fileJob.h"

/**
 * @brief Tree model of the job queue — one top-level row per job.
 *
 * A job with several sources (selected files / folders) gets one child row per
 * source, showing its own progress and status as if it had been copied on its
 * own. Single-source jobs have no children.
 *
 * All job state lives here. JobQueueWidget only reads via the model API
 * and never touches items directly.
 */
class JobModel : public QAbstractItemModel
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
    /// @brief Remaining time of every unfinished copy / move at the current speed —
    /// "≥ …" while a queued job's size is still unknown, empty without a speed yet.
    QString totalEtaString() const;
    /// @brief Speed used for estimates: the running job's, else the last one measured.
    double  assumedSpeed() const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int      rowCount(const QModelIndex &parent = {})    const override;
    int      columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void addJob(FileJob job);
    void removeFinishedJobs();
    /// @brief Remove one job — used for finished jobs that changed nothing.
    void removeJob(QUuid id);

    void setJobScanning(QUuid id);
    /// @p sourceBytes: size of every source, from the worker's scan.
    void setJobStarted(QUuid id, qint64 bytesTotal, const QList<qint64> &sourceBytes);
    /// @brief Sizes of a queued job's sources, scanned ahead of time for the
    /// remaining-time estimate. Ignored once the job has started.
    void setJobEstimate(QUuid id, const QList<qint64> &sourceBytes);
    /// @brief Pause / un-pause jobs that wait in the queue (the running job
    /// follows the worker's own signals). The worker never reports these.
    void setWaitingJobsPaused(bool paused);
    void setWaitingJobPaused(QUuid id, bool paused);
    /// @brief Called once per second: re-samples the running job's speed even
    /// when no progress arrives (a stall lowers it) and refreshes speed / time.
    void tick();
    void setJobProgress(QUuid id, qint64 bytesDone);
    void setJobPaused(QUuid id);
    void setJobResumed(QUuid id);
    void setJobFinished(QUuid id, FileJob::Status status, const QString &error);

    /// @brief Source @p index of job @p id started — its progress follows the job's from here.
    void setSourceStarted(QUuid id, int index, qint64 bytesTotal);
    /// @brief Source @p index of job @p id finished with @p status.
    void setSourceFinished(QUuid id, int index, FileJob::Status status);

private:
    /// One selected file / folder of a multi-source job — shown as a child row.
    struct SourceRow {
        QString         path;
        qint64          bytesTotal = 0;
        qint64          bytesDone  = 0;
        qint64          offset     = 0;    ///< Job's bytesDone when this source started.
        FileJob::Status status     = FileJob::Status::Queued;
    };

    /// One job plus its source rows. @c key is fixed for the job's lifetime —
    /// child indexes carry it as internalId, because rows shift when finished
    /// jobs are cleared. Top-level indexes have internalId 0.
    struct JobRow {
        FileJob            job;
        quintptr           key           = 0;
        QVector<SourceRow> sources;
        int                runningSource = -1;   ///< Index into sources, -1 = none.
        QElapsedTimer      runTimer;             ///< Started when the worker picks the job up.
        qint64             durationMs    = -1;   ///< Total time once finished, -1 = not finished.
    };

    /// @brief Resolve a job UUID to a model row index (-1 if not found).
    int  rowForJob(const QUuid &id) const;
    /// @brief Rebuild m_idToRow / m_keyToRow after rows were removed.
    void rebuildLookups();
    /// @brief True if child rows are shown for @p jobRow (more than one source).
    static bool hasSourceRows(const JobRow &jobRow);
    /// @brief Display data of source row @p sourceIndex.
    QVariant sourceData(const JobRow &jobRow, int sourceIndex, int column, int role) const;
    /// @brief Emit dataChanged for all columns of @p row.
    void emitRowChanged(int row);
    /// @brief Emit dataChanged for a column range of @p row.
    void emitRowChanged(int row, int firstCol, int lastCol);
    /// @brief Emit dataChanged for source row @p sourceIndex of job row @p row.
    void emitSourceChanged(int row, int sourceIndex);
    /// @brief Take a speed sample once the 1 s window has elapsed (smoothed).
    void sampleSpeed(FileJob &job);
    /// @brief Time column of a job row: remaining, estimated ("~") or total time.
    QVariant timeData(const JobRow &jobRow, int role) const;
    /// @brief "~M:SS" for @p bytes at assumedSpeed(), or empty without a speed.
    QString estimateString(qint64 bytes) const;

    QVector<JobRow>       m_rows;
    QHash<QUuid, int>     m_idToRow;
    QHash<quintptr, int>  m_keyToRow;
    quintptr              m_nextKey = 0;
    QSet<QUuid>           m_scanning;      ///< Jobs currently in calcTotalBytes phase.
    /// Cache for runningJob() — set by setJobScanning/setJobStarted,
    /// cleared by setJobFinished. Independent of FileWorkerThread::m_runningJobId:
    /// the worker tracks execution state, this tracks the model's view of it.
    QUuid                 m_runningJobId;
    /// Last speed measured (bytes/s) — kept across jobs so queued jobs and
    /// a paused job still get an estimate.
    double                m_lastSpeed = 0.0;
};

/// @brief Paints a native progress bar in the ColProgress column (no widget needed).
class ProgressBarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};
