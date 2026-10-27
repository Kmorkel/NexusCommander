#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QStyleOptionProgressBar>
#include "jobModel.h"
#include "fileWorkerThread.h"   // for kSpeedSampleWindowMs (single source of truth)
static const QString EmDash = QStringLiteral("—");

// Weight of a new speed sample. Smooths the jitter of single samples, but a
// slower link still shows in the remaining time within a few seconds.
static constexpr double kSpeedSmoothing = 0.5;

namespace {
QVariant statusColor(FileJob::Status status)
{
    switch (status) {
    case FileJob::Status::Running:          return QColor(0, 120, 212);
    case FileJob::Status::Paused:           return QColor(230, 168, 23);
    case FileJob::Status::Done:             return QColor(Qt::darkGreen);
    case FileJob::Status::Failed:           return QColor(Qt::red);
    case FileJob::Status::Cancelled:        return QColor(Qt::gray);
    case FileJob::Status::DoneWithWarnings:
    case FileJob::Status::Skipped:          return QColor(200, 130, 0);
    default: return {};
    }
}

bool isUnfinished(FileJob::Status status)
{
    return status == FileJob::Status::Queued
        || status == FileJob::Status::Running
        || status == FileJob::Status::Paused;
}
}

QString JobModel::formatDuration(qint64 ms)
{
    const int totalSecs = int(ms / 1000);
    const int hours = totalSecs / 3600;
    const int minutes = (totalSecs % 3600) / 60;
    const int seconds = totalSecs % 60;
    if (hours > 0)
        return QString("%1:%2:%3")
               .arg(hours).arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

QString JobModel::etaString(const FileJob &job)
{
    if (job.bytesDone <= 0 || job.bytesTotal <= 0
        || job.bytesDone >= job.bytesTotal || job.bytesPerSecond <= 0.0)
        return {};
    const qint64 remainingMs = static_cast<qint64>(
        (job.bytesTotal - job.bytesDone) / job.bytesPerSecond * 1000.0);
    return formatDuration(remainingMs);
}

double JobModel::assumedSpeed() const
{
    const FileJob *running = runningJob();
    if (running && running->bytesPerSecond > 0.0)
        return running->bytesPerSecond;
    return m_lastSpeed;
}

QString JobModel::estimateString(qint64 bytes) const
{
    const double speed = assumedSpeed();
    if (bytes <= 0 || speed <= 0.0)
        return {};
    return QStringLiteral("~") + formatDuration(static_cast<qint64>(bytes / speed * 1000.0));
}

QString JobModel::totalEtaString() const
{
    const double speed = assumedSpeed();
    if (speed <= 0.0)
        return {};

    qint64 remaining = 0;
    bool   complete  = true;
    for (const auto &jobRow : m_rows) {
        const FileJob &job = jobRow.job;
        // Delete progress counts items, not bytes — no time to add.
        if (!isUnfinished(job.status) || job.type == FileJob::Type::Delete)
            continue;
        if (job.bytesTotal <= 0)
            complete = false;   // queued, size not scanned yet
        else
            remaining += qMax<qint64>(0, job.bytesTotal - job.bytesDone);
    }

    const QString text = formatDuration(static_cast<qint64>(remaining / speed * 1000.0));
    return complete ? text : QStringLiteral("≥ ") + text;
}

JobModel::JobModel(QObject *parent)
    : QAbstractItemModel(parent)
{}

bool JobModel::hasSourceRows(const JobRow &jobRow)
{
    // A single file needs no breakdown — the job row already is that file.
    return jobRow.sources.size() > 1;
}

QModelIndex JobModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};
    if (!parent.isValid())
        return createIndex(row, column, quintptr(0));
    return createIndex(row, column, m_rows[parent.row()].key);
}

QModelIndex JobModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0)
        return {};
    const int row = m_keyToRow.value(child.internalId(), -1);
    return row < 0 ? QModelIndex() : createIndex(row, 0, quintptr(0));
}

int JobModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return static_cast<int>(m_rows.size());
    if (parent.internalId() != 0 || parent.column() != 0)
        return 0;   // source rows have no children
    const JobRow &jobRow = m_rows[parent.row()];
    return hasSourceRows(jobRow) ? static_cast<int>(jobRow.sources.size()) : 0;
}

int JobModel::columnCount(const QModelIndex & /*parent*/) const
{
    return ColCount;
}

QVariant JobModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    if (index.internalId() != 0) {
        const int row = m_keyToRow.value(index.internalId(), -1);
        if (row < 0 || index.row() >= m_rows[row].sources.size())
            return {};
        return sourceData(m_rows[row], index.row(), index.column(), role);
    }

    if (index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};

    const auto &job = m_rows[index.row()].job;
    const int col   = index.column();

    if (role == Qt::DisplayRole) {
        switch (col) {
        case ColType:     return job.typeString();
        case ColFiles:    return job.label();
        case ColProgress: {
            if (job.bytesTotal <= 0) return 0;
            return static_cast<int>(job.bytesDone * 100 / job.bytesTotal);
        }
        case ColSpeed:
            // Display boundary: convert internal bytes/s to MiB/s for the user.
            return job.bytesPerSecond > 0.0
                       ? QString::number(job.bytesPerSecond / kBytesPerMiB, 'f', 1)
                       : EmDash;
        case ColTime:
            return timeData(m_rows[index.row()], role);
        case ColStatus:
            if (m_scanning.contains(job.id) &&
                job.status != FileJob::Status::Paused) {
                return QObject::tr("Scanning...");
            }
            return job.statusString();
        }
    }

    // ProgressBarDelegate reads UserRole for custom progress text (e.g. "2/5 files" for delete jobs)

    if (role == Qt::UserRole && col == ColProgress) {
        if (job.type == FileJob::Type::Delete && job.bytesTotal > 0)
            return QObject::tr("%1/%2 files").arg(job.bytesDone).arg(job.bytesTotal);
        return QString();
    }

    if (role == Qt::ForegroundRole && col == ColStatus)
        return statusColor(job.status);

    if (role == Qt::ToolTipRole && col == ColTime)
        return timeData(m_rows[index.row()], role);

    if (role == Qt::ToolTipRole && col == ColFiles) {
        const QString fromLine = job.sources.value(0)
            + (job.sources.size() > 1
                   ? QObject::tr(" (+%1 more)").arg(job.sources.size() - 1)
                   : QString());
        const QString toLine = job.destination.isEmpty() ? QObject::tr("(delete)") : job.destination;
        QString tip = QObject::tr("From: %1\nTo:   %2").arg(fromLine, toLine);
        if (job.sources.size() > 1)
            tip += "\n\n" + QObject::tr("Double-click to show the files.");
        if (!job.errorMessage.isEmpty())
            tip += "\n\n" + QObject::tr("Error: %1").arg(job.errorMessage);
        return tip;
    }

    return {};
}

QVariant JobModel::timeData(const JobRow &jobRow, int role) const
{
    const FileJob &job = jobRow.job;
    const bool started = jobRow.runTimer.isValid();

    // Finished: how long the job took.
    if (!isUnfinished(job.status)) {
        if (role == Qt::ToolTipRole)
            return QObject::tr("Total time");
        return jobRow.durationMs >= 0 ? formatDuration(jobRow.durationMs) : QStringLiteral("0:00");
    }

    // Delete progress counts items, not bytes — no time to estimate.
    if (job.type == FileJob::Type::Delete)
        return role == Qt::DisplayRole ? QVariant(EmDash) : QVariant();

    if (job.status == FileJob::Status::Running && started) {
        if (role == Qt::ToolTipRole)
            return QObject::tr("Remaining at the current speed");
        // ETA is empty when bytesPerSecond is 0 (no sample yet) — show
        // an em-dash rather than a misleading "0:00" that implies "done".
        const QString eta = etaString(job);
        return eta.isEmpty() ? EmDash : eta;
    }

    // Queued, or paused: estimated at the speed of the running / last job.
    if (role == Qt::ToolTipRole) {
        const double speed = assumedSpeed();
        return speed > 0.0 ? QVariant(QObject::tr("Estimated at the current speed (%1 MB/s)")
                                          .arg(speed / kBytesPerMiB, 0, 'f', 1))
                           : QVariant();
    }
    const QString estimate = estimateString(job.bytesTotal - job.bytesDone);
    return estimate.isEmpty() ? EmDash : estimate;
}

QVariant JobModel::sourceData(const JobRow &jobRow, int sourceIndex, int column, int role) const
{
    const SourceRow &source = jobRow.sources[sourceIndex];

    // Remaining time of every file not done yet — follows the current speed.
    if (column == ColTime && (role == Qt::DisplayRole || role == Qt::ToolTipRole)) {
        if (!isUnfinished(source.status) || jobRow.job.type == FileJob::Type::Delete)
            return {};
        const bool current = sourceIndex == jobRow.runningSource
                             && jobRow.job.status == FileJob::Status::Running;
        if (role == Qt::ToolTipRole) {
            return current ? QObject::tr("Remaining at the current speed")
                           : QObject::tr("Estimated at the current speed");
        }
        QString estimate = estimateString(source.bytesTotal - source.bytesDone);
        if (current && estimate.startsWith('~'))
            estimate.remove(0, 1);   // this file is being copied right now — no guess
        return estimate;
    }

    if (role == Qt::DisplayRole) {
        switch (column) {
        case ColFiles: {
            const QString name = QFileInfo(source.path).fileName();
            return name.isEmpty() ? QDir::toNativeSeparators(source.path) : name;   // drive root
        }
        case ColProgress:
            if (source.bytesTotal > 0)
                return static_cast<int>(source.bytesDone * 100 / source.bytesTotal);
            // Empty file / folder: nothing to measure — done is done.
            return source.status == FileJob::Status::Done ? 100 : 0;
        case ColStatus:
            return FileJob::statusText(source.status);
        default:
            return {};   // type, speed and time belong to the job row
        }
    }

    if (role == Qt::ForegroundRole && column == ColStatus)
        return statusColor(source.status);

    if (role == Qt::ToolTipRole && column == ColFiles)
        return QDir::toNativeSeparators(source.path);

    return {};
}

QVariant JobModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColType:     return QObject::tr("Type");
    case ColFiles:    return QObject::tr("Files");
    case ColProgress: return QObject::tr("Progress");
    case ColSpeed:    return QObject::tr("MB/s");
    case ColTime:     return QObject::tr("Time");
    case ColStatus:   return QObject::tr("Status");
    }
    return {};
}

void JobModel::addJob(FileJob job)
{
    JobRow jobRow;
    jobRow.key = ++m_nextKey;
    for (const QString &path : std::as_const(job.sources))
        jobRow.sources.append(SourceRow{path});
    jobRow.job = std::move(job);

    const int row = static_cast<int>(m_rows.size());
    beginInsertRows({}, row, row);
    m_idToRow.insert(jobRow.job.id, row);
    m_keyToRow.insert(jobRow.key, row);
    m_rows.append(std::move(jobRow));
    endInsertRows();
}

void JobModel::removeFinishedJobs()
{
    for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
        if (isUnfinished(m_rows[i].job.status))
            continue;

        // Remove from the lookups before notifying views so that any code
        // triggered during endRemoveRows() sees a consistent state.
        m_idToRow.remove(m_rows[i].job.id);
        m_keyToRow.remove(m_rows[i].key);
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }

    // Row numbers shifted after each removal.
    rebuildLookups();
}

void JobModel::rebuildLookups()
{
    m_idToRow.clear();
    m_keyToRow.clear();
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        m_idToRow.insert(m_rows[i].job.id, i);
        m_keyToRow.insert(m_rows[i].key, i);
    }
}

void JobModel::setJobScanning(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_scanning.insert(id);

    // The worker picked the job up — it runs, whatever the model showed while
    // it waited (e.g. "Paused" from a Pause All that was resumed since). If
    // the worker keeps it paused, its jobPaused signal follows right after.
    JobRow &jobRow = m_rows[row];
    jobRow.job.status = FileJob::Status::Running;
    if (!jobRow.runTimer.isValid())
        jobRow.runTimer.start();
    m_runningJobId = id;
    emitRowChanged(row);
}

void JobModel::setJobStarted(QUuid id, qint64 bytesTotal, const QList<qint64> &sourceBytes)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_scanning.remove(id);
    JobRow &jobRow = m_rows[row];
    auto &job = jobRow.job;
    job.bytesTotal    = bytesTotal;
    job.speedTimer.start();
    job.lastBytesDone = 0;

    // Exact sizes from the worker's scan replace an estimate.
    for (int i = 0; i < jobRow.sources.size() && i < sourceBytes.size(); ++i) {
        jobRow.sources[i].bytesTotal = sourceBytes[i];
        emitSourceChanged(row, i);
    }

    // Delete jobs have no scan phase — started is their first signal.
    job.status = FileJob::Status::Running;
    if (!jobRow.runTimer.isValid())
        jobRow.runTimer.start();
    m_runningJobId = id;
    emitRowChanged(row);
}

void JobModel::setJobEstimate(QUuid id, const QList<qint64> &sourceBytes)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    // Too late: the worker's own scan (setJobStarted) is exact.
    JobRow &jobRow = m_rows[row];
    if (jobRow.runTimer.isValid() || !isUnfinished(jobRow.job.status))
        return;

    qint64 total = 0;
    for (int i = 0; i < jobRow.sources.size() && i < sourceBytes.size(); ++i) {
        jobRow.sources[i].bytesTotal = sourceBytes[i];
        total += sourceBytes[i];
        emitSourceChanged(row, i);
    }
    jobRow.job.bytesTotal = total;
    emitRowChanged(row);
}

void JobModel::setWaitingJobsPaused(bool paused)
{
    for (const auto &jobRow : std::as_const(m_rows))
        setWaitingJobPaused(jobRow.job.id, paused);
}

void JobModel::setWaitingJobPaused(QUuid id, bool paused)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    // The running job (also while paused) follows jobPaused / jobResumed.
    JobRow &jobRow = m_rows[row];
    if (jobRow.runTimer.isValid())
        return;

    FileJob::Status &status = jobRow.job.status;
    if (paused && status == FileJob::Status::Queued)
        status = FileJob::Status::Paused;
    else if (!paused && status == FileJob::Status::Paused)
        status = FileJob::Status::Queued;
    else
        return;
    emitRowChanged(row);
}

void JobModel::removeJob(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_scanning.remove(id);
    if (m_runningJobId == id)
        m_runningJobId = QUuid();

    beginRemoveRows({}, row, row);
    m_rows.removeAt(row);
    endRemoveRows();

    // Row numbers after the removed one shifted.
    rebuildLookups();
}

void JobModel::sampleSpeed(FileJob &job)
{
    // QElapsedTimer is monotonic and ~free per call (vs. QDateTime's syscall).
    const qint64 elapsedMs = job.speedTimer.elapsed();
    if (elapsedMs < kSpeedSampleWindowMs)
        return;

    const qint64 deltaBytes = job.bytesDone - job.lastBytesDone;
    const double sample     = double(deltaBytes) * 1000.0 / double(elapsedMs);
    // The first sample is taken as it is — later ones follow the trend, so the
    // remaining time drops / grows with the real speed instead of jumping.
    job.bytesPerSecond = job.bytesPerSecond > 0.0
                             ? kSpeedSmoothing * sample + (1.0 - kSpeedSmoothing) * job.bytesPerSecond
                             : sample;
    job.speedTimer.restart();
    job.lastBytesDone = job.bytesDone;

    if (job.bytesPerSecond > 0.0)
        m_lastSpeed = job.bytesPerSecond;
}

void JobModel::tick()
{
    if (m_rows.isEmpty())
        return;

    // No progress for a while (stalled network) must lower the speed too —
    // otherwise the remaining time would freeze at the last good value.
    const int row = rowForJob(m_runningJobId);
    if (row >= 0) {
        FileJob &job = m_rows[row].job;
        if (job.type != FileJob::Type::Delete && job.status == FileJob::Status::Running
            && !m_scanning.contains(job.id) && job.speedTimer.isValid())
            sampleSpeed(job);
    }

    // Every estimate depends on the current speed — refresh speed and time of all rows.
    const int lastRow = static_cast<int>(m_rows.size()) - 1;
    emit dataChanged(index(0, ColSpeed), index(lastRow, ColTime));
    for (int i = 0; i <= lastRow; ++i) {
        const JobRow &jobRow = m_rows[i];
        if (!hasSourceRows(jobRow) || !isUnfinished(jobRow.job.status))
            continue;
        const QModelIndex parentIndex = index(i, 0);
        const int lastSource = static_cast<int>(jobRow.sources.size()) - 1;
        emit dataChanged(index(0, ColTime, parentIndex), index(lastSource, ColTime, parentIndex));
    }
}

/// @brief Update progress and recalculate speed once per second.
///
/// Speed is sampled as deltaBytes / deltaTime over a 1-second window.
/// The baseline is seeded by setJobStarted / setJobResumed so the pause
/// duration never inflates the measurement.
void JobModel::setJobProgress(QUuid id, qint64 bytesDone)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    JobRow &jobRow = m_rows[row];
    auto &job = jobRow.job;
    job.bytesDone = bytesDone;

    // Sources run one after the other — the running one owns everything
    // the job did since it started.
    if (jobRow.runningSource >= 0) {
        SourceRow &source = jobRow.sources[jobRow.runningSource];
        source.bytesDone  = qBound<qint64>(0, bytesDone - source.offset, source.bytesTotal);
        emitSourceChanged(row, jobRow.runningSource);
    }

    // Delete tracks file count, not bytes — speed math doesn't apply.
    // Move runs the byte pipeline on cross-device fallbacks, so it keeps the
    // speed sampler; same-device renames simply never hit the 1 s window.
    if (job.type == FileJob::Type::Delete) {
        // Delete jobs only update the progress column on each tick.
        emitRowChanged(row, ColProgress, ColProgress);
        return;
    }

    sampleSpeed(job);

    // Only the time-varying columns change per progress tick. Skipping
    // ColType / ColFiles / ColStatus avoids re-painting unchanged cells
    // 20x/s — visible difference with many concurrent jobs in the queue.
    emitRowChanged(row, ColProgress, ColTime);
}

void JobModel::setJobPaused(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    JobRow &jobRow = m_rows[row];
    jobRow.job.status         = FileJob::Status::Paused;
    jobRow.job.bytesPerSecond = 0.0;
    emitRowChanged(row);

    if (jobRow.runningSource >= 0) {
        jobRow.sources[jobRow.runningSource].status = FileJob::Status::Paused;
        emitSourceChanged(row, jobRow.runningSource);
    }
}

void JobModel::setJobResumed(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    JobRow &jobRow = m_rows[row];
    auto &job = jobRow.job;
    job.status        = FileJob::Status::Running;
    // Reseed baseline so the pause duration isn't counted in the next measurement.
    job.speedTimer.restart();
    job.lastBytesDone = job.bytesDone;
    emitRowChanged(row);

    if (jobRow.runningSource >= 0) {
        jobRow.sources[jobRow.runningSource].status = FileJob::Status::Running;
        emitSourceChanged(row, jobRow.runningSource);
    }
}

void JobModel::setJobFinished(QUuid id, FileJob::Status status, const QString &error)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    JobRow &jobRow     = m_rows[row];
    auto &job          = jobRow.job;
    job.status         = status;
    job.errorMessage   = error;
    job.bytesPerSecond = 0.0;
    if (jobRow.runTimer.isValid())
        jobRow.durationMs = jobRow.runTimer.elapsed();

    // Skipped sources count as processed in the worker, so Done really means
    // every byte was handled — copied or deliberately skipped.
    if (status == FileJob::Status::Done || status == FileJob::Status::DoneWithWarnings)
        job.bytesDone = job.bytesTotal;

    // Sources the worker never got to: not started after a cancel or a failed
    // space check.
    const FileJob::Status leftOver = status == FileJob::Status::Failed ? FileJob::Status::Failed
                                                                       : FileJob::Status::Cancelled;
    for (int i = 0; i < jobRow.sources.size(); ++i) {
        if (isUnfinished(jobRow.sources[i].status)) {
            jobRow.sources[i].status = leftOver;
            emitSourceChanged(row, i);
        }
    }
    jobRow.runningSource = -1;

    // Only clear the cache if the finishing job is actually the one we tracked —
    // out-of-order signals must not erase a still-running job's id.
    if (m_runningJobId == id)
        m_runningJobId = QUuid();
    emitRowChanged(row);
}

void JobModel::setSourceStarted(QUuid id, int index, qint64 bytesTotal)
{
    const int row = rowForJob(id);
    if (row < 0 || index < 0 || index >= m_rows[row].sources.size()) return;

    JobRow    &jobRow = m_rows[row];
    SourceRow &source = jobRow.sources[index];
    source.status     = jobRow.job.status == FileJob::Status::Paused ? FileJob::Status::Paused
                                                                     : FileJob::Status::Running;
    source.bytesTotal = bytesTotal;
    source.bytesDone  = 0;
    source.offset     = jobRow.job.bytesDone;
    jobRow.runningSource = index;
    emitSourceChanged(row, index);
}

void JobModel::setSourceFinished(QUuid id, int index, FileJob::Status status)
{
    const int row = rowForJob(id);
    if (row < 0 || index < 0 || index >= m_rows[row].sources.size()) return;

    JobRow    &jobRow = m_rows[row];
    SourceRow &source = jobRow.sources[index];
    source.status     = status;
    if (status == FileJob::Status::Done || status == FileJob::Status::DoneWithWarnings
        || status == FileJob::Status::Skipped)
        source.bytesDone = source.bytesTotal;
    if (jobRow.runningSource == index)
        jobRow.runningSource = -1;
    emitSourceChanged(row, index);
}


QUuid JobModel::jobIdAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return {};
    return m_rows[row].job.id;
}

const FileJob *JobModel::jobAt(int row) const
{
    if (row < 0 || row >= m_rows.size()) return nullptr;
    return &m_rows[row].job;
}

const FileJob *JobModel::jobById(QUuid id) const
{
    return jobAt(rowForJob(id));
}

FileJob::Status JobModel::jobStatusAt(int row) const
{
    if (row < 0 || row >= m_rows.size()) return FileJob::Status::Queued;
    return m_rows[row].job.status;
}

int JobModel::activeJobCount() const
{
    int count = 0;
    for (const auto &jobRow : m_rows)
        if (isUnfinished(jobRow.job.status))
            ++count;
    return count;
}

int JobModel::jobCount() const
{
    return static_cast<int>(m_rows.size());
}

int JobModel::runningJobIndex() const
{
    int index = 1;
    for (const auto &jobRow : m_rows) {
        const auto status = jobRow.job.status;
        if (status == FileJob::Status::Running)
            return index;

        if (status == FileJob::Status::Done ||
            status == FileJob::Status::Failed ||
            status == FileJob::Status::Cancelled ||
            status == FileJob::Status::DoneWithWarnings)
            index++;
    }
    return -1;
}

const FileJob *JobModel::runningJob() const
{
    const int row = rowForJob(m_runningJobId);
    if (row < 0) return nullptr;
    return &m_rows[row].job;
}


int JobModel::rowForJob(const QUuid &id) const
{
    return m_idToRow.value(id, -1);
}

void JobModel::emitRowChanged(int row)
{
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

void JobModel::emitRowChanged(int row, int firstCol, int lastCol)
{
    emit dataChanged(index(row, firstCol), index(row, lastCol));
}

void JobModel::emitSourceChanged(int row, int sourceIndex)
{
    if (!hasSourceRows(m_rows[row]))
        return;   // no child rows shown for a single source
    const QModelIndex parentIndex = index(row, 0);
    emit dataChanged(index(sourceIndex, 0, parentIndex), index(sourceIndex, ColCount - 1, parentIndex));
}


void ProgressBarDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const int percent = index.data(Qt::DisplayRole).toInt();
    const QString customText = index.data(Qt::UserRole).toString();

    QStyleOptionProgressBar opt;
    opt.rect        = option.rect;
    opt.minimum     = 0;
    opt.maximum     = 100;
    opt.progress    = percent;
    opt.text        = customText.isEmpty() ? QString("%1%").arg(percent) : customText;
    opt.textVisible = true;

    QApplication::style()->drawControl(QStyle::CE_ProgressBar, &opt, painter);
}
