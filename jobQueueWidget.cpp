#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProcess>
#include <QMessageBox>
#include <QMenu>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QDirIterator>
#include <QPromise>
#include <QtConcurrent/QtConcurrentRun>
#include "fileSystemUtils.h"
#include "jobQueueWidget.h"

static const QString EmDash = QStringLiteral("—");

namespace {
// Speed, remaining times and the status label refresh at this rate.
constexpr int kTickIntervalMs = 1000;

// Size of a file or folder for the estimate — same rules as the worker's scan:
// hidden / system files included, links and junctions not followed, .part files
// left out if the job skips them.
qint64 estimateTreeSize(const QFileInfo &info, const QPromise<QList<qint64>> &promise,
                        bool skipPartFiles)
{
    if (isLink(info))
        return 0;
    if (skipPartFiles && !info.isDir() && isPartFileName(info.fileName()))
        return 0;
    if (!info.isDir())
        return info.size();

    qint64 size = 0;
    QDirIterator it(info.absoluteFilePath(), kAllEntries);
    while (it.hasNext()) {
        if (promise.isCanceled())
            return size;   // result is dropped anyway
        it.next();
        size += estimateTreeSize(it.fileInfo(), promise, skipPartFiles);
    }
    return size;
}

// Pool-thread entry point: one size per source. Blocking — can take a while on a network drive.
void estimateSourceBytes(QPromise<QList<qint64>> &promise, const QStringList &sources,
                         bool skipPartFiles)
{
    QList<qint64> sizes;
    sizes.reserve(sources.size());
    for (const QString &source : sources) {
        if (promise.isCanceled())
            return;
        sizes << estimateTreeSize(QFileInfo(source), promise, skipPartFiles);
    }
    promise.addResult(sizes);
}

// "Apply to all" of the conflict / read-only dialog counts only for an explicit
// click — X / Esc (no button) must not apply "Skip" to every remaining file,
// and "Cancel Job" ends the job anyway.
bool appliesToAll(const QAbstractButton *clicked, const QAbstractButton *cancelButton,
                  const QCheckBox *applyAllBox)
{
    return clicked && clicked != cancelButton && applyAllBox->isChecked();
}
}

JobQueueWidget::JobQueueWidget(FileWorkerThread *worker, QWidget *parent)
    : QDialog(parent)
    , m_worker(worker)
    , m_model(new JobModel(this))
{
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
    setWindowTitle(tr("Job Queue"));
    resize(720, 420);
    setupUi();
    connectSignals();
}

JobQueueWidget::~JobQueueWidget()
{
    // Let running estimate scans stop at their next entry — they never touch the widget.
    for (QFuture<QList<qint64>> &future : m_estimates)
        future.cancel();
}

void JobQueueWidget::showRestored()
{
    // show() alone leaves a minimized window minimized — clear only that flag,
    // so a window that was maximized before comes back maximized.
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
}

void JobQueueWidget::setQueuePaused(bool paused)
{
    if (paused == m_worker->isGlobalPaused())
        return;
    if (paused)
        onPauseAll();
    else
        onResumeAll();
}

void JobQueueWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    m_pauseBanner = new QLabel(this);
    m_pauseBanner->setText(tr("  Queue paused -- add your jobs, then click \"Start All\" to begin."));
    m_pauseBanner->setStyleSheet(
        "QLabel {"
        "  background-color: #FFF3CD;"
        "  color: #856404;"
        "  border: 1px solid #FFEEBA;"
        "  border-radius: 4px;"
        "  padding: 6px 10px;"
        "  font-weight: bold;"
        "}");
    m_pauseBanner->setVisible(false);
    mainLayout->addWidget(m_pauseBanner);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegateForColumn(JobModel::ColProgress, new ProgressBarDelegate(m_view));

    QHeaderView *header = m_view->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(JobModel::ColFiles,    QHeaderView::Stretch);
    header->setSectionResizeMode(JobModel::ColProgress, QHeaderView::Fixed);
    header->setSectionResizeMode(JobModel::ColSpeed,    QHeaderView::Fixed);
    header->setSectionResizeMode(JobModel::ColTime,     QHeaderView::Fixed);
    header->setSectionResizeMode(JobModel::ColType,     QHeaderView::Fixed);
    header->setSectionResizeMode(JobModel::ColStatus,   QHeaderView::Fixed);
    m_view->setColumnWidth(JobModel::ColProgress, 130);
    m_view->setColumnWidth(JobModel::ColSpeed,     65);
    m_view->setColumnWidth(JobModel::ColTime,      65);
    m_view->setColumnWidth(JobModel::ColType,      60);
    m_view->setColumnWidth(JobModel::ColStatus,   110);

    // Expand arrow and indentation in the Files column — the file rows sit
    // visibly under their job's name. Double-click expands (QTreeView default).
    m_view->setTreePosition(JobModel::ColFiles);
    m_view->setRootIsDecorated(true);
    m_view->setItemsExpandable(true);
    m_view->setExpandsOnDoubleClick(true);
    m_view->setUniformRowHeights(true);
    m_view->setAllColumnsShowFocus(true);

    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setAlternatingRowColors(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    mainLayout->addWidget(m_view, 1);

    m_statusLabel = new QLabel(tr("No jobs"), this);
    m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
    mainLayout->addWidget(m_statusLabel);

    auto *speedRow = new QHBoxLayout;
    speedRow->addWidget(new QLabel(tr("Speed limit:"), this));
    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setRange(0.0, 10000.0);
    m_speedSpin->setValue(0.0);
    m_speedSpin->setSuffix(tr(" MB/s"));
    m_speedSpin->setSpecialValueText(tr("Unlimited"));
    m_speedSpin->setFixedWidth(130);
    speedRow->addWidget(m_speedSpin);
    speedRow->addStretch();
    mainLayout->addLayout(speedRow);

    auto *btnRow = new QHBoxLayout;

    m_pauseAllBtn  = new QPushButton(QIcon::fromTheme("media-playback-pause"),
                                     tr("Pause All"),   this);
    m_resumeAllBtn = new QPushButton(QIcon::fromTheme("media-playback-start"),
                                     tr("Resume All"),  this);
    m_cancelAllBtn = new QPushButton(QIcon::fromTheme("process-stop"),
                                     tr("Cancel All"),  this);
    m_clearBtn     = new QPushButton(QIcon::fromTheme("edit-clear"),
                                     tr("Clear finished"), this);

    // A QDialog presses its first auto-default button on Enter — confirming the
    // speed limit with Enter paused every job. No button reacts to Enter here.
    for (QPushButton *button : {m_pauseAllBtn, m_resumeAllBtn, m_cancelAllBtn, m_clearBtn}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    btnRow->addWidget(m_pauseAllBtn);
    btnRow->addWidget(m_resumeAllBtn);
    btnRow->addWidget(m_cancelAllBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_clearBtn);
    mainLayout->addLayout(btnRow);

    m_shutdownBox = new QCheckBox(tr("Shutdown PC when all jobs finish"), this);
    mainLayout->addWidget(m_shutdownBox);
}

void JobQueueWidget::connectSignals()
{
    // Worker → Widget
    connect(m_worker, &FileWorkerThread::jobScanning,
            this, &JobQueueWidget::onJobScanning);
    connect(m_worker, &FileWorkerThread::jobStarted,
            this, &JobQueueWidget::onJobStarted);
    connect(m_worker, &FileWorkerThread::jobProgress,
            this, &JobQueueWidget::onJobProgress);
    connect(m_worker, &FileWorkerThread::jobPaused,
            this, &JobQueueWidget::onJobPaused);
    connect(m_worker, &FileWorkerThread::jobResumed,
            this, &JobQueueWidget::onJobResumed);
    connect(m_worker, &FileWorkerThread::jobFinished,
            this, &JobQueueWidget::onJobFinished);
    connect(m_worker, &FileWorkerThread::allJobsDone,
            this, &JobQueueWidget::onAllJobsDone);
    connect(m_worker, &FileWorkerThread::conflictDetected,
            this, &JobQueueWidget::onConflictDetected);
    connect(m_worker, &FileWorkerThread::readOnlyDetected,
            this, &JobQueueWidget::onReadOnlyDetected);
    connect(m_worker, &FileWorkerThread::insufficientSpace,
            this, &JobQueueWidget::onInsufficientSpace);

    // Buttons
    connect(m_pauseAllBtn,  &QPushButton::clicked, this, &JobQueueWidget::onPauseAll);
    connect(m_resumeAllBtn, &QPushButton::clicked, this, &JobQueueWidget::onResumeAll);
    connect(m_cancelAllBtn, &QPushButton::clicked, this, &JobQueueWidget::onCancelAll);
    connect(m_clearBtn,     &QPushButton::clicked, this, &JobQueueWidget::onClearFinished);

    connect(m_speedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &JobQueueWidget::onSpeedLimitChanged);

    // Worker → Model: per-file rows of multi-file jobs (no extra UI work needed).
    connect(m_worker, &FileWorkerThread::sourceStarted,
            m_model,  &JobModel::setSourceStarted);
    connect(m_worker, &FileWorkerThread::sourceFinished,
            m_model,  &JobModel::setSourceFinished);

    // Right-click on table
    connect(m_view, &QTreeView::customContextMenuRequested,
            this, &JobQueueWidget::onContextMenuRequested);

    // Runs only while jobs are active — started in addJob(), stopped when the queue is idle.
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickIntervalMs);
    connect(m_tickTimer, &QTimer::timeout, this, [this] {
        m_model->tick();
        updateOverallStatus();
    });
}

void JobQueueWidget::addJob(FileJob job)
{
    const QUuid jobId = job.id;
    // Queued behind another job: scan its size now, so the total remaining
    // time covers it before the worker gets to it.
    const bool waits = m_model->activeJobCount() > 0;
    if (waits)
        startEstimate(job);
    m_model->addJob(std::move(job));

    // Pre-mark as Paused in the model for immediate UI feedback when global
    // pause is active. run() pauses the worker when it dequeues the job.
    if (m_worker->isGlobalPaused())
        m_model->setJobPaused(jobId);

    ++m_batchTotal;
    m_tickTimer->start();

    // Not shown here — popping up on every F5 / F6 got in the way. The status-bar
    // button in the main window shows the job; failures open the dialog.
    updateTitle();
    m_statusLabel->setText(tr("%n job(s) in queue", nullptr, m_model->jobCount()));
    updateTaskbar();
    emit jobAdded();
}

void JobQueueWidget::startEstimate(const FileJob &job)
{
    // Delete progress counts items — no size needed.
    if (job.type == FileJob::Type::Delete)
        return;

    const QUuid jobId = job.id;
    QFuture<QList<qint64>> future = QtConcurrent::run(&estimateSourceBytes, job.sources,
                                                      job.skipPartFiles);
    m_estimates.insert(jobId, future);

    // Context `this`: the continuation is dropped if the widget is already gone.
    future.then(this, [this, jobId](QFuture<QList<qint64>> finished) {
        m_estimates.remove(jobId);
        if (!finished.isCanceled() && finished.resultCount() > 0)
            m_model->setJobEstimate(jobId, finished.result());
    });
}

void JobQueueWidget::cancelEstimate(QUuid jobId)
{
    auto it = m_estimates.find(jobId);
    if (it == m_estimates.end())
        return;
    it->cancel();
    m_estimates.erase(it);
}

void JobQueueWidget::onJobScanning(QUuid jobId)
{
    cancelEstimate(jobId);
    m_model->setJobScanning(jobId);
}

void JobQueueWidget::onJobStarted(QUuid jobId, qint64 bytesTotal, const QList<qint64> &sourceBytes)
{
    cancelEstimate(jobId);   // Delete jobs have no scanning signal
    m_model->setJobStarted(jobId, bytesTotal, sourceBytes);
}

void JobQueueWidget::onJobProgress(QUuid jobId, qint64 bytesDone)
{
    // The status label follows m_tickTimer — progress ticks arrive at up to
    // 20 Hz, refreshing it that often is wasted work and flickers.
    m_model->setJobProgress(jobId, bytesDone);
}

void JobQueueWidget::onJobPaused(QUuid jobId)
{
    m_model->setJobPaused(jobId);
    updateOverallStatus();
}

void JobQueueWidget::onJobResumed(QUuid jobId)
{
    m_model->setJobResumed(jobId);
    updateOverallStatus();
}

void JobQueueWidget::onJobFinished(QUuid jobId, FileJob::Status status,
                                    const QString &error, bool unchanged)
{
    cancelEstimate(jobId);
    m_model->setJobFinished(jobId, status, error);

    // Nothing written, moved or deleted — cancelled before it started, or every
    // item skipped. Such a job tells the user nothing; a failure always stays.
    // MainWindow::onJobFinished() runs after this slot and then finds no job —
    // fine, there is nothing to refresh.
    if (unchanged && status != FileJob::Status::Failed)
        m_model->removeJob(jobId);

    ++m_batchFinished;
    if (status == FileJob::Status::Failed) {
        m_batchFailed = true;
        // The only case that opens the dialog by itself — the user has to see why.
        showRestored();
    } else if (!unchanged && (status == FileJob::Status::Done
                              || status == FileJob::Status::DoneWithWarnings)) {
        ++m_batchDone;
    }

    // Queue idle: the next job starts a new batch. Checked here, not only in
    // onAllJobsDone() — a queue of jobs cancelled before start never emits that.
    if (m_model->activeJobCount() == 0) {
        emit batchFinished(m_batchDone, m_batchFailed);
        m_batchTotal    = 0;
        m_batchFinished = 0;
        m_batchDone     = 0;
        m_batchFailed   = false;
        m_tickTimer->stop();
    }

    updateTitle();
    updateOverallStatus();
}

void JobQueueWidget::onAllJobsDone()
{
    updateTitle();

    // Every job changed nothing and was removed — nothing to report.
    if (m_model->jobCount() == 0) {
        m_statusLabel->setText(tr("No jobs"));
        m_shutdownBox->setChecked(false);
        return;
    }

    // Walk the model once to figure out the actual outcome — saying
    // "All jobs finished ✓" after a Cancel All would be a lie.
    int done      = 0;
    int cancelled = 0;
    int failed    = 0;
    int warnings  = 0;

    for (int row = 0; row < m_model->jobCount(); ++row) {
        switch (m_model->jobStatusAt(row)) {
        case FileJob::Status::Done:             ++done;     break;
        case FileJob::Status::Cancelled:        ++cancelled; break;
        case FileJob::Status::Failed:           ++failed;   break;
        case FileJob::Status::DoneWithWarnings: ++warnings; break;
        default: break;
        }
    }

    QString msg;
    if (cancelled > 0 && done == 0 && failed == 0)
        msg = tr("Cancelled (%n job(s))", nullptr, cancelled);
    else if (failed > 0 && done == 0)
        msg = tr("Failed (%n job(s))", nullptr, failed);
    else if (cancelled > 0 || failed > 0)
        msg = tr("Finished — %1 done, %2 cancelled, %3 failed")
                  .arg(done).arg(cancelled).arg(failed);
    else if (warnings > 0)
        msg = tr("All jobs finished (%1 with warnings)").arg(warnings);
    else
        msg = tr("All jobs finished \u2713");

    m_statusLabel->setText(msg);

    const bool successfulRun = (done + warnings > 0) && (cancelled == 0) && (failed == 0);
    if (!successfulRun)
        m_shutdownBox->setChecked(false);

    if (m_shutdownBox->isChecked()) {
        const auto btn = QMessageBox::question(this, tr("Shutdown"),
            tr("All file operations finished.\nShutdown PC now?"),
            QMessageBox::Yes | QMessageBox::No);
        // Re-check active jobs: a new job could have been enqueued while the
        // confirmation dialog was open. Don't shut down with work in flight.
        if (btn == QMessageBox::Yes && m_model->activeJobCount() == 0) {
#ifdef Q_OS_WIN
            QProcess::startDetached("shutdown", {"/s", "/t", "0"});
#else
            QProcess::startDetached("shutdown", {"-h", "now"});
#endif
        }
        m_shutdownBox->setChecked(false);
    }
}

void JobQueueWidget::onConflictDetected(QUuid jobId, const QString &fileName,
                                         const QString &destPath, bool readOnly)
{
    QMessageBox dlg(this);
    dlg.setWindowTitle(tr("File already exists"));
    dlg.setText(tr("\"%1\" already exists in:\n%2")
                    .arg(fileName, QDir::toNativeSeparators(destPath)));
    // The existing file is write-protected — overwriting replaces protected content.
    dlg.setInformativeText(readOnly ? tr("The existing file is read-only.\nWhat would you like to do?")
                                    : tr("What would you like to do?"));
    dlg.setIcon(QMessageBox::Question);

    auto *overwriteBtn = dlg.addButton(tr("Overwrite"),  QMessageBox::AcceptRole);
    auto *keepBothBtn  = dlg.addButton(tr("Keep Both"),  QMessageBox::AcceptRole);
    dlg.addButton(tr("Skip"), QMessageBox::RejectRole);  // No variable — Skip is the default fallback.
    auto *cancelBtn    = dlg.addButton(tr("Cancel Job"), QMessageBox::DestructiveRole);

    auto *applyAll = new QCheckBox(tr("Apply to all remaining files"), &dlg);
    dlg.setCheckBox(applyAll);

    dlg.exec();

    const auto *clickedButton = dlg.clickedButton();

    // Resolve which action was chosen (X / ESC defaults to Skip)
    FileJob::ConflictAction action = FileJob::ConflictAction::Skip;

    if      (clickedButton == overwriteBtn) action = FileJob::ConflictAction::Overwrite;
    else if (clickedButton == keepBothBtn)  action = FileJob::ConflictAction::KeepBoth;
    else if (clickedButton == cancelBtn)    m_worker->cancelJob(jobId);

    m_worker->resolveConflict(action, appliesToAll(clickedButton, cancelBtn, applyAll));
}

void JobQueueWidget::onReadOnlyDetected(QUuid jobId, const QString &path, bool isMove)
{
    const QString name = QFileInfo(path).fileName();

    QMessageBox dlg(this);
    dlg.setWindowTitle(tr("Read-only"));
    dlg.setIcon(QMessageBox::Warning);
    if (isMove) {
        dlg.setText(QFileInfo(path).isDir()
                        ? tr("The folder \"%1\" contains read-only files.").arg(name)
                        : tr("\"%1\" is read-only.").arg(name));
        dlg.setInformativeText(tr("Read-only files are protected against changes.\n"
                                  "Move anyway, or only copy and leave the original where it is?"));
    } else {
        dlg.setText(tr("\"%1\" is read-only.").arg(name));
        dlg.setInformativeText(tr("Delete it anyway?"));
    }

    auto *proceedBtn  = dlg.addButton(isMove ? tr("Move anyway") : tr("Delete anyway"),
                                      QMessageBox::AcceptRole);
    QAbstractButton *copyOnlyBtn = isMove ? dlg.addButton(tr("Copy only"), QMessageBox::AcceptRole)
                                          : nullptr;
    dlg.addButton(tr("Skip"), QMessageBox::RejectRole);  // also X / Esc — never touches the file
    auto *cancelBtn   = dlg.addButton(tr("Cancel Job"), QMessageBox::DestructiveRole);

    auto *applyAll = new QCheckBox(tr("Apply to all remaining read-only files"), &dlg);
    dlg.setCheckBox(applyAll);

    dlg.exec();

    const auto *clickedButton = dlg.clickedButton();

    FileJob::ReadOnlyAction action = FileJob::ReadOnlyAction::Skip;
    if      (clickedButton == proceedBtn)                  action = FileJob::ReadOnlyAction::Proceed;
    else if (copyOnlyBtn && clickedButton == copyOnlyBtn)  action = FileJob::ReadOnlyAction::CopyOnly;
    else if (clickedButton == cancelBtn)                   m_worker->cancelJob(jobId);

    m_worker->resolveReadOnly(action, appliesToAll(clickedButton, cancelBtn, applyAll));
}

void JobQueueWidget::onInsufficientSpace(QUuid jobId, const QString &destination,
                                         qint64 bytesNeeded, qint64 bytesAvailable)
{
    // The worker does not wait for this dialog — the job is already marked Failed.
    const FileJob *job = m_model->jobById(jobId);
    QMessageBox::warning(this, tr("Not enough space"),
                         tr("%1 was not started — the destination is too small.\n\n"
                            "Destination: %2\nNeeded: %3\nFree: %4\n\n"
                            "Nothing has been written. Free up space and start it again.")
                             .arg(job ? job->label() : tr("The job"),
                                  QDir::toNativeSeparators(destination),
                                  formatSize(bytesNeeded), formatSize(bytesAvailable)));
}

void JobQueueWidget::onPauseAll()
{
    m_worker->pauseAll();
    // The worker only reports the running job — waiting ones are marked here.
    m_model->setWaitingJobsPaused(true);
    updatePauseUi();
    updateOverallStatus();
}

void JobQueueWidget::onResumeAll()
{
    m_worker->resumeAll();
    // Waiting jobs go back to Queued; the running one gets jobResumed.
    m_model->setWaitingJobsPaused(false);
    updatePauseUi();
    updateOverallStatus();
}

void JobQueueWidget::onCancelAll()
{
    const auto btn = QMessageBox::question(this, tr("Cancel All"),
        tr("Cancel all running and queued jobs?"),
        QMessageBox::Yes | QMessageBox::No);
    if (btn == QMessageBox::Yes) {
        m_worker->cancelAll();
        // cancelAll() also lifts every pause — waiting jobs are no longer paused.
        m_model->setWaitingJobsPaused(false);
        // cancelAll() resets the global pause flag — make the banner / button
        // labels reflect the new state immediately.
        updatePauseUi();
    }
}

void JobQueueWidget::onClearFinished()
{
    m_model->removeFinishedJobs();
    updateOverallStatus();
}

void JobQueueWidget::onSpeedLimitChanged(double value)
{
    m_worker->setSpeedLimitMBs(value);
}

void JobQueueWidget::addJobControlActions(QMenu &menu, FileJob::Status status, QUuid jobId)
{
    // A waiting job is never reported by the worker — the model is updated here
    // (setWaitingJobPaused() leaves the running job to the worker's signals).
    if (status == FileJob::Status::Running || status == FileJob::Status::Queued) {
        menu.addAction(QIcon::fromTheme("media-playback-pause"), tr("Pause"),
                       [this, jobId]{
                           m_worker->pauseJob(jobId);
                           m_model->setWaitingJobPaused(jobId, true);
                       });
    }
    if (status == FileJob::Status::Paused) {
        menu.addAction(QIcon::fromTheme("media-playback-start"), tr("Resume"),
                       [this, jobId]{
                           m_worker->resumeJob(jobId);
                           m_model->setWaitingJobPaused(jobId, false);
                       });
    }
    if (status == FileJob::Status::Running ||
        status == FileJob::Status::Paused  ||
        status == FileJob::Status::Queued)
    {
        menu.addAction(QIcon::fromTheme("process-stop"), tr("Cancel"),
                       [this, jobId]{ m_worker->cancelJob(jobId); });
    }
}

void JobQueueWidget::onContextMenuRequested(const QPoint &pos)
{
    QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid()) return;

    // A file row acts on the job it belongs to.
    if (index.parent().isValid())
        index = index.parent();

    const int   row    = index.row();
    const QUuid jobId  = m_model->jobIdAt(row);
    const auto  status = m_model->jobStatusAt(row);

    QMenu menu(this);
    addJobControlActions(menu, status, jobId);

    if (!menu.isEmpty())
        menu.addSeparator();

    menu.addAction(QIcon::fromTheme("dialog-information"), tr("Details..."),
                   [this, row]{ showJobDetails(row); });

    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void JobQueueWidget::updatePauseUi()
{
    const bool paused = m_worker->isGlobalPaused();
    m_pauseBanner->setVisible(paused);
    m_pauseAllBtn->setEnabled(!paused);
    m_resumeAllBtn->setEnabled(paused);

    if (paused) {
        m_resumeAllBtn->setText(tr("Start All"));
        m_resumeAllBtn->setStyleSheet(
            "QPushButton {"
            "  background-color: #28A745;"
            "  color: white;"
            "  font-weight: bold;"
            "  border: 1px solid #1E7E34;"
            "  border-radius: 3px;"
            "  padding: 4px 12px;"
            "}"
            "QPushButton:hover { background-color: #218838; }");
    } else {
        m_resumeAllBtn->setText(tr("Resume All"));
        m_resumeAllBtn->setStyleSheet({});
    }
    emit pauseChanged(paused);
}

void JobQueueWidget::showJobDetails(int row)
{
    const FileJob *job = m_model->jobAt(row);
    if (!job) return;

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Job Details -- %1").arg(job->typeString()));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(560, 300);

    auto *layout = new QVBoxLayout(dlg);
    auto *form   = new QFormLayout;

    auto *srcEdit = new QPlainTextEdit(job->sources.join("\n"), dlg);
    srcEdit->setReadOnly(true);
    srcEdit->setMinimumHeight(80);
    form->addRow(tr("From:"), srcEdit);

    auto *dstEdit = new QLineEdit(
        job->destination.isEmpty() ? tr("(delete -- no destination)") : job->destination, dlg);
    dstEdit->setReadOnly(true);
    form->addRow(tr("To:"), dstEdit);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::accept);
    layout->addWidget(buttons);

    dlg->exec();
}

void JobQueueWidget::updateOverallStatus()
{
    updateTaskbar();

    if (m_model->jobCount() == 0) {
        m_statusLabel->setText(tr("No jobs"));
        return;
    }

    const int activeJobs = m_model->activeJobCount();

    if (activeJobs <= 0) {
        m_statusLabel->setText(tr("All done"));
        return;
    }

    const int totalJobs  = m_model->jobCount();
    const int currentJob = m_model->runningJobIndex();

    // No running job (everything queued/paused) — runningJobIndex returns -1.
    if (currentJob < 0) {
        m_statusLabel->setText(tr("%1 job(s) waiting").arg(activeJobs));
        return;
    }

    // Both at the current speed — they grow when the transfer slows down.
    const QString eta   = calculateEtaString();
    const QString total = m_model->totalEtaString();

    m_statusLabel->setText(
        tr("Job %1/%2  %3  this job %4  %3  all jobs %5")
            .arg(currentJob)
            .arg(totalJobs)
            .arg(EmDash, eta, total.isEmpty() ? EmDash : total));
}

void JobQueueWidget::updateTaskbar()
{
    const int activeJobs = m_model->activeJobCount();
    if (m_batchTotal == 0 || activeJobs == 0) {
        emit taskbarProgressChanged(TaskbarProgress::State::None, 0.0);
        emit activityChanged(0, TaskbarProgress::State::None, 0.0, {});
        return;
    }

    // Finished jobs of the batch plus the share of the running one.
    const FileJob *running = m_model->runningJob();
    double runningShare = 0.0;
    if (running && running->bytesTotal > 0)
        runningShare = qBound(0.0, double(running->bytesDone) / double(running->bytesTotal), 1.0);
    const double fraction = (m_batchFinished + runningShare) / m_batchTotal;

    TaskbarProgress::State state = TaskbarProgress::State::Normal;
    if (m_worker->isGlobalPaused() || (running && running->status == FileJob::Status::Paused))
        state = TaskbarProgress::State::Paused;
    else if (m_batchFailed)
        state = TaskbarProgress::State::Error;

    emit taskbarProgressChanged(state, fraction);
    emit activityChanged(activeJobs, state, fraction, m_model->totalEtaString());
}

void JobQueueWidget::updateTitle()
{
    const int active = m_model->activeJobCount();
    setWindowTitle(active > 0
        ? tr("Job Queue  \u2013  %1 active").arg(active)
        : tr("Job Queue"));
}

QString JobQueueWidget::calculateEtaString() const
{
    const FileJob *running = m_model->runningJob();
    if (!running)
        return EmDash;
    const QString eta = JobModel::etaString(*running);
    return eta.isEmpty() ? EmDash : eta;
}

int JobQueueWidget::activeJobCount() const
{
    return m_model->activeJobCount();
}

QStringList JobQueueWidget::affectedDirsForJob(QUuid jobId) const
{
    const FileJob *job = m_model->jobById(jobId);
    if (!job) return {};

    QStringList dirs;
    // A copy leaves its source folders unchanged — refreshing them re-read a
    // network source on the GUI thread while the next job kept the NAS busy.
    if (job->type != FileJob::Type::Copy) {
        for (const QString &src : job->sources)
            dirs << QFileInfo(src).absolutePath();
    }
    if (!job->destination.isEmpty())
        dirs << job->destination;

    dirs.removeDuplicates();
    return dirs;
}