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
#include "jobQueueWidget.h"

static const QString EmDash = QStringLiteral("—");
JobQueueWidget::JobQueueWidget(FileWorkerThread *worker, QWidget *parent)
    : QDialog(parent)
    , m_worker(worker)
    , m_model(new JobModel(this))
{
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
    setWindowTitle("Job Queue");
    resize(720, 420);
    setupUi();
    connectSignals();
}

void JobQueueWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    m_pauseBanner = new QLabel(this);
    m_pauseBanner->setText("  Queue paused -- add your jobs, then click \"Start All\" to begin.");
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

    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegateForColumn(JobModel::ColProgress, new ProgressBarDelegate(m_view));

    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColFiles,    QHeaderView::Stretch);
    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColProgress, QHeaderView::Fixed);
    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColSpeed,    QHeaderView::Fixed);
    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColTime,     QHeaderView::Fixed);
    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColType,     QHeaderView::Fixed);
    m_view->horizontalHeader()->setSectionResizeMode(JobModel::ColStatus,   QHeaderView::Fixed);
    m_view->setColumnWidth(JobModel::ColProgress, 130);
    m_view->setColumnWidth(JobModel::ColSpeed,     65);
    m_view->setColumnWidth(JobModel::ColTime,      65);
    m_view->setColumnWidth(JobModel::ColType,      60);
    m_view->setColumnWidth(JobModel::ColStatus,    80);

    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->verticalHeader()->setVisible(false);
    m_view->setAlternatingRowColors(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    mainLayout->addWidget(m_view, 1);

    m_statusLabel = new QLabel("No jobs", this);
    m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
    mainLayout->addWidget(m_statusLabel);

    auto *speedRow = new QHBoxLayout;
    speedRow->addWidget(new QLabel("Speed limit:", this));
    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setRange(0.0, 10000.0);
    m_speedSpin->setValue(0.0);
    m_speedSpin->setSuffix(" MB/s");
    m_speedSpin->setSpecialValueText("Unlimited");
    m_speedSpin->setFixedWidth(130);
    speedRow->addWidget(m_speedSpin);
    speedRow->addStretch();
    mainLayout->addLayout(speedRow);

    auto *btnRow = new QHBoxLayout;

    m_pauseAllBtn  = new QPushButton(QIcon::fromTheme("media-playback-pause"),
                                     "Pause All",   this);
    m_resumeAllBtn = new QPushButton(QIcon::fromTheme("media-playback-start"),
                                     "Resume All",  this);
    m_cancelAllBtn = new QPushButton(QIcon::fromTheme("process-stop"),
                                     "Cancel All",  this);
    m_clearBtn     = new QPushButton(QIcon::fromTheme("edit-clear"),
                                     "Clear finished", this);

    btnRow->addWidget(m_pauseAllBtn);
    btnRow->addWidget(m_resumeAllBtn);
    btnRow->addWidget(m_cancelAllBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_clearBtn);
    mainLayout->addLayout(btnRow);

    m_shutdownBox = new QCheckBox("Shutdown PC when all jobs finish", this);
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

    // Buttons
    connect(m_pauseAllBtn,  &QPushButton::clicked, this, &JobQueueWidget::onPauseAll);
    connect(m_resumeAllBtn, &QPushButton::clicked, this, &JobQueueWidget::onResumeAll);
    connect(m_cancelAllBtn, &QPushButton::clicked, this, &JobQueueWidget::onCancelAll);
    connect(m_clearBtn,     &QPushButton::clicked, this, &JobQueueWidget::onClearFinished);

    connect(m_speedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &JobQueueWidget::onSpeedLimitChanged);

    // Right-click on table
    connect(m_view, &QTableView::customContextMenuRequested,
            this, &JobQueueWidget::onContextMenuRequested);
}

void JobQueueWidget::addJob(FileJob job)
{
    const QUuid jobId = job.id;
    m_model->addJob(std::move(job));

    // Pre-mark as Paused in the model for immediate UI feedback when global
    // pause is active. run() pauses the worker when it dequeues the job.
    if (m_worker->isGlobalPaused())
        m_model->setJobPaused(jobId);

    updateTitle();
    if (!isVisible()) show();
    m_statusLabel->setText(QString("%1 job(s) in queue").arg(m_model->jobCount()));
}

void JobQueueWidget::onJobScanning(QUuid jobId)
{
    m_model->setJobScanning(jobId);
}

void JobQueueWidget::onJobStarted(QUuid jobId, qint64 bytesTotal)
{
    m_model->setJobStarted(jobId, bytesTotal);
}

void JobQueueWidget::onJobProgress(QUuid jobId, qint64 bytesDone)
{
    m_model->setJobProgress(jobId, bytesDone);

    // Throttle: progress ticks arrive at ~20 Hz, but the status label only
    // shows job index + ETA — refreshing it more than once per second is wasted
    // work on the GUI thread and causes visible flicker.
    if (!m_statusThrottle.isValid() || m_statusThrottle.elapsed() >= 1000) {
        updateOverallStatus();
        m_statusThrottle.restart();
    }
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
                                    const QString &error)
{
    m_model->setJobFinished(jobId, status, error);
    updateTitle();
    updateOverallStatus();
}

void JobQueueWidget::onAllJobsDone()
{
    updateTitle();

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
        msg = QString("Cancelled (%1 job%2)").arg(cancelled).arg(cancelled == 1 ? "" : "s");
    else if (failed > 0 && done == 0)
        msg = QString("Failed (%1 job%2)").arg(failed).arg(failed == 1 ? "" : "s");
    else if (cancelled > 0 || failed > 0)
        msg = QString("Finished — %1 done, %2 cancelled, %3 failed")
                  .arg(done).arg(cancelled).arg(failed);
    else if (warnings > 0)
        msg = QString("All jobs finished (%1 with warnings)").arg(warnings);
    else
        msg = QString("All jobs finished \u2713");

    m_statusLabel->setText(msg);

    const bool successfulRun = (done + warnings > 0) && (cancelled == 0) && (failed == 0);
    if (!successfulRun)
        m_shutdownBox->setChecked(false);

    if (m_shutdownBox->isChecked()) {
        const auto btn = QMessageBox::question(this, "Shutdown",
            "All file operations finished.\nShutdown PC now?",
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
                                         const QString &destPath)
{
    QMessageBox dlg(this);
    dlg.setWindowTitle("File already exists");
    dlg.setText(QString("\"%1\" already exists in:\n%2")
                    .arg(fileName, QDir::toNativeSeparators(destPath)));
    dlg.setInformativeText("What would you like to do?");
    dlg.setIcon(QMessageBox::Question);

    auto *overwriteBtn = dlg.addButton("Overwrite",  QMessageBox::AcceptRole);
    auto *keepBothBtn  = dlg.addButton("Keep Both",  QMessageBox::AcceptRole);
    dlg.addButton("Skip", QMessageBox::RejectRole);  // No variable — Skip is the default fallback.
    auto *cancelBtn    = dlg.addButton("Cancel Job", QMessageBox::DestructiveRole);

    auto *applyAll = new QCheckBox("Apply to all remaining files", &dlg);
    dlg.setCheckBox(applyAll);

    dlg.exec();

    const auto *clickedButton = dlg.clickedButton();

    // Resolve which action was chosen (X / ESC defaults to Skip)
    FileJob::ConflictAction action = FileJob::ConflictAction::Skip;

    if      (clickedButton == overwriteBtn) action = FileJob::ConflictAction::Overwrite;
    else if (clickedButton == keepBothBtn)  action = FileJob::ConflictAction::KeepBoth;
    else if (clickedButton == cancelBtn)    m_worker->cancelJob(jobId);

    // Require an explicit click: dismissing with X/ESC must not propagate "Skip"
    // to every remaining conflict in the job.
    const bool applyToAll = (clickedButton != nullptr)
                            && (clickedButton != cancelBtn)
                            && applyAll->isChecked();
    m_worker->resolveConflict(action, applyToAll);
}

void JobQueueWidget::onPauseAll()
{
    m_worker->pauseAll();
    updatePauseUi();
}

void JobQueueWidget::onResumeAll()
{
    m_worker->resumeAll();
    updatePauseUi();
}

void JobQueueWidget::onCancelAll()
{
    const auto btn = QMessageBox::question(this, "Cancel All",
        "Cancel all running and queued jobs?",
        QMessageBox::Yes | QMessageBox::No);
    if (btn == QMessageBox::Yes) {
        m_worker->cancelAll();
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
    if (status == FileJob::Status::Running) {
        menu.addAction(QIcon::fromTheme("media-playback-pause"), "Pause",
                       [this, jobId]{ m_worker->pauseJob(jobId); });
    }
    if (status == FileJob::Status::Paused) {
        menu.addAction(QIcon::fromTheme("media-playback-start"), "Resume",
                       [this, jobId]{ m_worker->resumeJob(jobId); });
    }
    if (status == FileJob::Status::Running ||
        status == FileJob::Status::Paused  ||
        status == FileJob::Status::Queued)
    {
        menu.addAction(QIcon::fromTheme("process-stop"), "Cancel",
                       [this, jobId]{ m_worker->cancelJob(jobId); });
    }
}

void JobQueueWidget::onContextMenuRequested(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid()) return;

    const int   row    = index.row();
    const QUuid jobId  = m_model->jobIdAt(row);
    const auto  status = m_model->jobStatusAt(row);

    QMenu menu(this);
    addJobControlActions(menu, status, jobId);

    if (!menu.isEmpty())
        menu.addSeparator();

    menu.addAction(QIcon::fromTheme("dialog-information"), "Details...",
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
        m_resumeAllBtn->setText("Start All");
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
        m_resumeAllBtn->setText("Resume All");
        m_resumeAllBtn->setStyleSheet({});
    }
}

void JobQueueWidget::showJobDetails(int row)
{
    const FileJob *job = m_model->jobAt(row);
    if (!job) return;

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(QString("Job Details -- %1").arg(job->typeString()));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(560, 300);

    auto *layout = new QVBoxLayout(dlg);
    auto *form   = new QFormLayout;

    auto *srcEdit = new QPlainTextEdit(job->sources.join("\n"), dlg);
    srcEdit->setReadOnly(true);
    srcEdit->setMinimumHeight(80);
    form->addRow("From:", srcEdit);

    auto *dstEdit = new QLineEdit(
        job->destination.isEmpty() ? "(delete -- no destination)" : job->destination, dlg);
    dstEdit->setReadOnly(true);
    form->addRow("To:", dstEdit);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::accept);
    layout->addWidget(buttons);

    dlg->exec();
}

void JobQueueWidget::updateOverallStatus()
{
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

    const QString eta    = calculateEtaString();

    m_statusLabel->setText(
        tr("Job %1/%2 %3 ETA %4")
            .arg(currentJob)
            .arg(totalJobs)
            .arg(EmDash, eta));
}

void JobQueueWidget::updateTitle()
{
    const int active = m_model->activeJobCount();
    setWindowTitle(active > 0
        ? QString("Job Queue  \u2013  %1 active").arg(active)
        : "Job Queue");
}

QString JobQueueWidget::calculateEtaString() const
{
    const FileJob *running = m_model->runningJob();
    if (!running)
        return EmDash;
    const QString eta = JobModel::etaString(*running);
    return eta.isEmpty() ? EmDash : eta;
}

QStringList JobQueueWidget::affectedDirsForJob(QUuid jobId) const
{
    const FileJob *job = m_model->jobById(jobId);
    if (!job) return {};

    QStringList dirs;
    for (const QString &src : job->sources)
        dirs << QFileInfo(src).absolutePath();
    if (!job->destination.isEmpty())
        dirs << job->destination;

    dirs.removeDuplicates();
    return dirs;
}