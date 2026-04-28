#include <QApplication>
#include <QPainter>
#include <QStyleOptionProgressBar>
#include "jobModel.h"
#include "fileWorkerThread.h"   // for kSpeedSampleWindowMs (single source of truth)
static const QString EmDash = QStringLiteral("—");

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

JobModel::JobModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int JobModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_jobs.size());
}

int JobModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant JobModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_jobs.size()))
        return {};

    const auto &job = m_jobs[index.row()];
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
        case ColTime: {
            const auto status = job.status;
            if (status == FileJob::Status::Running) {
                // ETA is empty when bytesPerSecond is 0 (no sample yet) — show
                // an em-dash rather than a misleading "0:00" that implies "done".
                const QString eta = etaString(job);
                return eta.isEmpty() ? EmDash : eta;
            }
            // Queued / Paused: no data yet.
            if (status == FileJob::Status::Queued || status == FileJob::Status::Paused)
                return EmDash;
            return QStringLiteral("0:00");
        }
        case ColStatus:
            if (m_scanning.contains(job.id) &&
                job.status != FileJob::Status::Paused) {
                return QStringLiteral("Scanning...");
            }
            return job.statusString();
        }
    }

    // ProgressBarDelegate reads UserRole for custom progress text (e.g. "2/5 files" for delete jobs)

    if (role == Qt::UserRole && col == ColProgress) {
        if (job.type == FileJob::Type::Delete && job.bytesTotal > 0)
            return QString("%1/%2 files").arg(job.bytesDone).arg(job.bytesTotal);
        return QString();
    }

    if (role == Qt::ForegroundRole && col == ColStatus) {
        switch (job.status) {
        case FileJob::Status::Running:          return QColor(0, 120, 212);
        case FileJob::Status::Paused:           return QColor(230, 168, 23);
        case FileJob::Status::Done:             return QColor(Qt::darkGreen);
        case FileJob::Status::Failed:           return QColor(Qt::red);
        case FileJob::Status::Cancelled:        return QColor(Qt::gray);
        case FileJob::Status::DoneWithWarnings: return QColor(200, 130, 0);
        default: return {};
        }
    }

    if (role == Qt::ToolTipRole && col == ColFiles) {
        const QString fromLine = job.sources.value(0)
            + (job.sources.size() > 1
                   ? QString(" (+%1 more)").arg(job.sources.size() - 1)
                   : QString());
        const QString toLine = job.destination.isEmpty() ? "(delete)" : job.destination;
        QString tip = QString("From: %1\nTo:   %2").arg(fromLine, toLine);
        if (!job.errorMessage.isEmpty())
            tip += QString("\n\nError: %1").arg(job.errorMessage);
        return tip;
    }

    return {};
}

QVariant JobModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColType:     return QStringLiteral("Type");
    case ColFiles:    return QStringLiteral("Files");
    case ColProgress: return QStringLiteral("Progress");
    case ColSpeed:    return QStringLiteral("MB/s");
    case ColTime:     return QStringLiteral("Time");
    case ColStatus:   return QStringLiteral("Status");
    }
    return {};
}

void JobModel::addJob(FileJob job)
{
    const int row = static_cast<int>(m_jobs.size());
    beginInsertRows({}, row, row);
    m_idToRow.insert(job.id, row);
    m_jobs.append(std::move(job));
    endInsertRows();
}

void JobModel::removeFinishedJobs()
{
    for (int i = static_cast<int>(m_jobs.size()) - 1; i >= 0; --i) {
        const auto status = m_jobs[i].status;
        if (status == FileJob::Status::Queued ||
            status == FileJob::Status::Running ||
            status == FileJob::Status::Paused)
            continue;

        // Remove from the lookup before notifying views so that any code
        // triggered during endRemoveRows() sees a consistent m_idToRow.
        m_idToRow.remove(m_jobs[i].id);
        beginRemoveRows({}, i, i);
        m_jobs.removeAt(i);
        endRemoveRows();
    }

    // Rebuild index cache — row numbers shifted after each removal.
    m_idToRow.clear();
    for (int i = 0; i < static_cast<int>(m_jobs.size()); ++i)
        m_idToRow.insert(m_jobs[i].id, i);
}

void JobModel::setJobScanning(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_scanning.insert(id);

    if (m_jobs[row].status != FileJob::Status::Paused)
        m_jobs[row].status = FileJob::Status::Running;
    m_runningJobId = id;
    emitRowChanged(row);
}

void JobModel::setJobStarted(QUuid id, qint64 bytesTotal)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_scanning.remove(id);
    auto &job = m_jobs[row];
    job.bytesTotal    = bytesTotal;
    job.speedTimer.start();
    job.lastBytesDone = 0;

    if (job.status != FileJob::Status::Paused)
        job.status = FileJob::Status::Running;
    m_runningJobId = id;
    emitRowChanged(row);
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

    auto &job = m_jobs[row];
    job.bytesDone = bytesDone;

    // Delete tracks file count, not bytes — speed math doesn't apply.
    // Move runs the byte pipeline on cross-device fallbacks, so it keeps the
    // speed sampler; same-device renames simply never hit the 1 s window.
    if (job.type == FileJob::Type::Delete) {
        // Delete jobs only update the progress column on each tick.
        emitRowChanged(row, ColProgress, ColProgress);
        return;
    }

    // QElapsedTimer is monotonic and ~free per call (vs. QDateTime's syscall).
    const qint64 elapsedMs = job.speedTimer.elapsed();
    if (elapsedMs >= kSpeedSampleWindowMs) {
        const qint64 deltaBytes = bytesDone - job.lastBytesDone;
        job.bytesPerSecond = double(deltaBytes) * 1000.0 / double(elapsedMs);
        job.speedTimer.restart();
        job.lastBytesDone  = bytesDone;
    }

    // Only the time-varying columns change per progress tick. Skipping
    // ColType / ColFiles / ColStatus avoids re-painting unchanged cells
    // 20x/s — visible difference with many concurrent jobs in the queue.
    emitRowChanged(row, ColProgress, ColTime);
}

void JobModel::setJobPaused(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    m_jobs[row].status         = FileJob::Status::Paused;
    m_jobs[row].bytesPerSecond = 0.0;
    emitRowChanged(row);
}

void JobModel::setJobResumed(QUuid id)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    auto &job = m_jobs[row];
    job.status        = FileJob::Status::Running;
    // Reseed baseline so the pause duration isn't counted in the next measurement.
    job.speedTimer.restart();
    job.lastBytesDone = job.bytesDone;
    emitRowChanged(row);
}

void JobModel::setJobFinished(QUuid id, FileJob::Status status, const QString &error)
{
    const int row = rowForJob(id);
    if (row < 0) return;

    auto &job          = m_jobs[row];
    job.status         = status;
    job.errorMessage   = error;
    job.bytesPerSecond = 0.0;

    if (status == FileJob::Status::Done || status == FileJob::Status::DoneWithWarnings)
        job.bytesDone = job.bytesTotal;

    // Only clear the cache if the finishing job is actually the one we tracked —
    // out-of-order signals must not erase a still-running job's id.
    if (m_runningJobId == id)
        m_runningJobId = QUuid();
    emitRowChanged(row);
}


QUuid JobModel::jobIdAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_jobs.size())) return {};
    return m_jobs[row].id;
}

const FileJob *JobModel::jobAt(int row) const
{
    if (row < 0 || row >= m_jobs.size()) return nullptr;
    return &m_jobs[row];
}

const FileJob *JobModel::jobById(QUuid id) const
{
    return jobAt(rowForJob(id));
}

FileJob::Status JobModel::jobStatusAt(int row) const
{
    if (row < 0 || row >= m_jobs.size()) return FileJob::Status::Queued;
    return m_jobs[row].status;
}

int JobModel::activeJobCount() const
{
    int count = 0;
    for (const auto &job : m_jobs)
        if (job.status == FileJob::Status::Queued ||
            job.status == FileJob::Status::Running ||
            job.status == FileJob::Status::Paused)
            ++count;
    return count;
}

int JobModel::jobCount() const
{
    return static_cast<int>(m_jobs.size());
}

int JobModel::runningJobIndex() const
{
    int index = 1;
    for (const auto &job : m_jobs) {
        if (job.status == FileJob::Status::Running)
            return index;

        if (job.status == FileJob::Status::Done ||
            job.status == FileJob::Status::Failed ||
            job.status == FileJob::Status::Cancelled ||
            job.status == FileJob::Status::DoneWithWarnings)
            index++;
    }
    return -1;
}

const FileJob *JobModel::runningJob() const
{
    const int row = rowForJob(m_runningJobId);
    if (row < 0) return nullptr;
    return &m_jobs[row];
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
