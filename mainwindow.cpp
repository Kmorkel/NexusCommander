#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QSet>
#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>
#include <QToolButton>
#include <QTabBar>
#include <QToolBar>
#include <QGridLayout>
#include <QSpacerItem>
#include <QStyle>
#include <QSettings>
#include <QScreen>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include "clipboardUtils.h"
#include "fileSystemUtils.h"

#ifdef _WIN32
#include <windows.h>
#include <dbt.h>
#endif

namespace {
// On Windows, WM_DEVICECHANGE catches most arrivals/removals, so the timer is
// just a slow fallback for network/virtual drives that don't fire it.
// On other platforms the timer is the only signal, so it polls more frequently.
#ifdef _WIN32
constexpr int kDriveWatchIntervalMs = 30000;
#else
constexpr int kDriveWatchIntervalMs = 2000;
#endif

// Job indicator in the status bar: progress bar resolution and width.
constexpr int kJobProgressSteps = 1000;
constexpr int kJobProgressWidth = 140;
// Highlight after a job was queued, and how long the outcome stays after the queue went idle.
constexpr int kJobFlashMs       = 1000;
constexpr int kJobDoneVisibleMs = 4000;

// A saved window place counts only if at least this much of it is on the monitor.
constexpr int kMinVisibleWidth  = 100;
constexpr int kMinVisibleHeight = 50;

/// @brief Tab title for @p path: the folder name, or the path itself for a root like "C:/".
QString tabLabel(const QString &path)
{
    const QString dirName = QDir(path).dirName();
    return dirName.isEmpty() ? path : dirName;
}

/// @return Every FilePanel tab in @p tabs.
QList<FilePanel *> panelsIn(const QTabWidget *tabs)
{
    QList<FilePanel *> panels;
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *panel = qobject_cast<FilePanel *>(tabs->widget(i)))
            panels << panel;
    }
    return panels;
}

/// @brief Build a display string for confirmation dialogs (up to @p maxShown names).
QString formatFileNamesText(const QStringList &paths, int maxShown = 5)
{
    QStringList names;
    for (const QString &filePath : paths)
        names << QFileInfo(filePath).fileName();

    QString text = names.mid(0, maxShown).join("\n");
    if (paths.size() > maxShown)
        text += "\n" + QObject::tr("… and %1 more").arg(paths.size() - maxShown);
    return text;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    // Intercept the Tab key before Qt's focus chain captures it.
    qApp->installEventFilter(this);

    setupWindow();
    setupCore();
    setupUiComponents();
    setupConnections();
    setupDriveWatcher();
    restoreSettings();
}

MainWindow::~MainWindow()
{
    // Panels before m_folderSizeCache — child objects die in creation order,
    // and the cache was created first.
    delete m_splitter;
    delete ui;
}

void MainWindow::setupPanels()
{
    m_leftTabs  = new QTabWidget(this);
    m_rightTabs = new QTabWidget(this);

    initTabWidget(m_leftTabs);
    initTabWidget(m_rightTabs);

    FilePanel *leftPanel = addTab(m_leftTabs, QDir::homePath());
    addTab(m_rightTabs, QDir::homePath());

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_leftTabs);
    m_splitter->addWidget(m_rightTabs);
    m_splitter->setSizes({600, 600});

    setCentralWidget(m_splitter);

    // Start with the left panel focused (window not shown yet — Qt keeps
    // this as the focus widget and applies it on activation).
    leftPanel->setFocus();
}

void MainWindow::connectPanel(FilePanel *panel, QTabWidget *side)
{
    connect(panel, &FilePanel::statusMessage, this, &MainWindow::onStatusUpdate);
    connect(panel, &FilePanel::deleteRequested, this, &MainWindow::onDeleteRequested);
    connect(panel, &FilePanel::pasteRequested, this, [this, panel]{ onPasteRequested(panel); });

    connect(panel, &FilePanel::pathChanged, this, [side, panel](const QString &path){
        const int idx = side->indexOf(panel);
        if (idx < 0) return;

        side->setTabText(idx, tabLabel(path));
        side->setTabToolTip(idx, QDir::toNativeSeparators(path));
    });

    connect(panel, &FilePanel::openInNewTabRequested, this, [this, side](const QString &path){
        addTab(side, path);
    });

    // The shared drive scan in MainWindow feeds every panel — panels never
    // query drives themselves. Opening a dropdown requests a probing rescan.
    connect(this, &MainWindow::drivesChanged,
            panel, &FilePanel::setDrives);
    connect(panel, &FilePanel::driveScanRequested, this, [this]{
        requestDriveScan(/*probeNetwork=*/true);
    });
    panel->setDrives(m_drives);

    connect(panel, &FilePanel::networkSearchRequested,
            m_networkDiscovery, &NetworkDiscovery::start);
    connect(panel, &FilePanel::networkSharesFound,
            this,  &MainWindow::onSharesFound);
    connect(m_networkDiscovery, &NetworkDiscovery::runningChanged,
            panel, &FilePanel::setNetworkSearchRunning);
    panel->setNetworkSearchRunning(m_networkDiscovery->isRunning());
}

void MainWindow::createActions()
{
    m_newTabAction = new QAction(tr("New &Tab"), this);
    m_newTabAction->setShortcut(Qt::CTRL | Qt::Key_T);
    connect(m_newTabAction, &QAction::triggered, this, [this]{
        QTabWidget *side = activeSide();
        FilePanel *panel = addTab(side);
        side->setCurrentWidget(panel);
        panel->setFocus();
    });

    m_closeTabAction = new QAction(tr("&Close Tab"), this);
    m_closeTabAction->setShortcut(Qt::CTRL | Qt::Key_W);
    connect(m_closeTabAction, &QAction::triggered, this, [this]{
        QTabWidget *side = activeSide();
        closeTab(side, side->currentIndex());
    });

    // ThemeIcon enum, not XDG names — Qt maps only these to Windows' Segoe Fluent
    // Icons; an unknown name ("document-copy") silently gives no icon at all.
    m_copyAction = new QAction(QIcon::fromTheme(QIcon::ThemeIcon::EditCopy), tr("F5 Copy"), this);
    m_copyAction->setShortcut(Qt::Key_F5);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::onCopyRequested);

    // Scissors, like Cut in Explorer — Qt has no "move" icon.
    m_moveAction = new QAction(QIcon::fromTheme(QIcon::ThemeIcon::EditCut), tr("F6 Move"), this);
    m_moveAction->setShortcut(Qt::Key_F6);
    connect(m_moveAction, &QAction::triggered, this, &MainWindow::onMoveRequested);

    m_newFolderAction = new QAction(QIcon::fromTheme("folder-new"), tr("F7 New Folder"), this);
    m_newFolderAction->setShortcut(Qt::Key_F7);
    connect(m_newFolderAction, &QAction::triggered, this, &MainWindow::onNewFolderRequested);

    m_deleteAction = new QAction(QIcon::fromTheme("edit-delete"), tr("F8 Delete"), this);
    m_deleteAction->setShortcut(Qt::Key_F8);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::onDeleteRequested);

    m_refreshAction = new QAction(QIcon::fromTheme("view-refresh"), tr("F2 Refresh"), this);
    m_refreshAction->setShortcut(Qt::Key_F2);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::onRefreshRequested);

    // Clock: jobs wait in the queue — also the status-bar button's icon, next to the time left.
    m_jobQueueAction = new QAction(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpenRecent), tr("Job Queue"), this);
    connect(m_jobQueueAction, &QAction::triggered, m_jobQueue, &JobQueueWidget::showRestored);

    // Also usable with an empty queue — jobs added while paused wait for Resume.
    m_pauseQueueAction = new QAction(this);
    connect(m_pauseQueueAction, &QAction::triggered, this, [this]{
        m_jobQueue->setQueuePaused(!m_worker->isGlobalPaused());
    });
    updatePauseQueueAction(m_worker->isGlobalPaused());

    m_terminalAction = new QAction(QIcon::fromTheme("utilities-terminal"), tr("F9 Terminal"), this);
    m_terminalAction->setShortcut(Qt::Key_F9);
    m_terminalAction->setToolTip(tr("Open a terminal in the folder of the active panel"));
    connect(m_terminalAction, &QAction::triggered, this, [this]{
        if (FilePanel *panel = activePanel())
            panel->openTerminal(panel->currentPath());
    });

    m_showHiddenAction = new QAction(tr("Show &Hidden Files"), this);
    m_showHiddenAction->setCheckable(true);
    connect(m_showHiddenAction, &QAction::toggled,
            this, &MainWindow::onToggleHiddenFiles);

    m_hidePartFilesAction = new QAction(tr("Hide .&part Files"), this);
    m_hidePartFilesAction->setCheckable(true);
    // On by default — set before the connect, the panels do not exist yet (addTab() applies it).
    m_hidePartFilesAction->setChecked(m_hidePartFiles);
    m_hidePartFilesAction->setToolTip(tr("Hide unfinished downloads (*.part) — copy and move leave them out"));
    connect(m_hidePartFilesAction, &QAction::toggled,
            this, &MainWindow::onToggleHidePartFiles);
}

void MainWindow::initTabWidget(QTabWidget *tabs)
{
    tabs->setTabPosition(QTabWidget::North);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->setDocumentMode(true);
    tabs->tabBar()->setAutoHide(false);

    tabs->setStyleSheet(
        "QTabBar::close-button {"
        "  width: 12px; height: 12px;"
        "  padding: 0px; margin: 1px;"
        "}"
        );

    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index){
        closeTab(tabs, index);
    });
}

FilePanel *MainWindow::addTab(QTabWidget *side, const QString &path)
{
    const QString startPath = path.isEmpty() ? QDir::homePath() : path;

    auto *panel = new FilePanel(startPath, m_folderSizeCache, side);
    connectPanel(panel, side);
    // Same view as the existing tabs — the defaults need no call (and no refresh).
    if (m_showHiddenFiles)
        panel->setShowHiddenFiles(true);
    if (m_hidePartFiles)
        panel->setHidePartFiles(true);

    const int idx = side->addTab(panel, tabLabel(startPath));
    side->setTabToolTip(idx, QDir::toNativeSeparators(startPath));

    // Single tab: hide close button so the user can't close the last tab.
    // Two tabs: restore the close button on tab 0 (it was hidden when alone)
    // and force the tab bar visible (some Qt styles hide it for one tab).
    if (side->count() == 1) {
        side->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    } else if (side->count() == 2) {
        side->setTabsClosable(false); // without this line last tab will not have a close icon!
        side->setTabsClosable(true);
    }

    return panel;
}

void MainWindow::closeTab(QTabWidget *side, int index)
{
    if (side->count() <= 1) return;

    QWidget *removed = side->widget(index);
    side->removeTab(index);

    if (side->count() == 1)
        side->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);

    // Restore focus so activeSide() keeps working
    if (auto *panel = qobject_cast<FilePanel *>(side->currentWidget()))
        panel->setFocus();

    removed->deleteLater();
}

void MainWindow::setupToolBar()
{
    QToolBar *bar = addToolBar(tr("Main"));
    bar->setMovable(false);

    bar->addAction(m_copyAction);
    bar->addAction(m_moveAction);
    bar->addAction(m_newFolderAction);
    bar->addAction(m_deleteAction);
    bar->addSeparator();
    bar->addAction(m_refreshAction);
    bar->addAction(m_terminalAction);
    bar->addSeparator();

    bar->addAction(m_jobQueueAction);
    bar->addAction(m_pauseQueueAction);
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = ui->menubar->addMenu(tr("&File"));
    fileMenu->addAction(m_newTabAction);
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_newFolderAction);
    fileMenu->addAction(m_terminalAction);
    fileMenu->addSeparator();
    // close(), not qApp->quit() — goes through closeEvent() and its running-jobs warning.
    fileMenu->addAction(tr("&Quit\tAlt+F4"), this, &QWidget::close);

    QMenu *viewMenu = ui->menubar->addMenu(tr("&View"));
    viewMenu->addAction(m_refreshAction);

    viewMenu->addAction(m_showHiddenAction);
    viewMenu->addAction(m_hidePartFilesAction);

    QMenu *helpMenu = ui->menubar->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, [this]{
        QMessageBox::about(this, tr("About NexusCommander"),
                           tr("<h2>NexusCommander</h2>"
                              "<p>Version %1 &nbsp;&ndash;&nbsp; "
                              R"(<a href="https://github.com/Kmorkel/NexusCommander/releases">Check for updates</a></p>)"
                              "<p>A fast, dual-pane file manager for Windows,<br>"
                              "inspired by Total Commander.</p>"
                              "<p>Licensed under the "
                              R"(<a href="https://github.com/Kmorkel/NexusCommander/blob/main/LICENSE">MIT License</a>)"
                              "<p><b>Shortcuts:</b><br>"
                              "F2 Refresh<br>"
                              "F5 Copy &nbsp; F6 Move<br>"
                              "F7 New Folder &nbsp; F8 Delete<br>"
                              "F9 Terminal<br>"
                              "Ctrl+T New Tab &nbsp; Ctrl+W Close Tab<br>"
                              "Ctrl+S Filter &nbsp; Tab Switch Panel</p>"
                              "<hr>"
                              "<p><b>Third-Party Software</b></p>"
                              "<p>This application uses the <a href=\"https://www.qt.io/\">Qt Framework</a> "
                              "(version %2), licensed under the "
                              "<a href=\"https://www.gnu.org/licenses/lgpl-3.0.html\">LGPL v3</a>. "
                              "Qt is linked dynamically; no modifications were made to the Qt source code.</p>"
                              "<p>Qt source code is available at "
                              "<a href=\"https://www.qt.io/download-open-source\">qt.io/download-open-source</a>.</p>")
                               .arg(QApplication::applicationVersion(),
                                    QLatin1String(qVersion())));
    });
}

void MainWindow::setupStatusBar()
{
    m_statusLabel = new QLabel("", this);
    ui->statusbar->addWidget(m_statusLabel, 1);

    // Running jobs stay visible when the Job Queue window is closed — a click reopens it.
    m_jobStatusButton = new QToolButton(this);
    m_jobStatusButton->setAutoRaise(true);
    m_jobStatusButton->setFocusPolicy(Qt::NoFocus);
    m_jobStatusButton->setToolTip(tr("Show Job Queue"));
    // Icon: reads as a button, not as plain status text.
    m_jobStatusButton->setIcon(m_jobQueueAction->icon());
    m_jobStatusButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_jobStatusButton, &QToolButton::clicked, m_jobQueue, &JobQueueWidget::showRestored);

    m_jobFlashTimer = new QTimer(this);
    m_jobFlashTimer->setSingleShot(true);
    m_jobFlashTimer->setInterval(kJobFlashMs);
    connect(m_jobFlashTimer, &QTimer::timeout, this, [this]{ m_jobStatusButton->setStyleSheet({}); });

    m_jobDoneTimer = new QTimer(this);
    m_jobDoneTimer->setSingleShot(true);
    m_jobDoneTimer->setInterval(kJobDoneVisibleMs);
    connect(m_jobDoneTimer, &QTimer::timeout, m_jobStatusButton, &QWidget::hide);

    m_jobProgressBar = new QProgressBar(this);
    m_jobProgressBar->setRange(0, kJobProgressSteps);
    m_jobProgressBar->setFixedWidth(kJobProgressWidth);
    m_jobProgressBar->setTextVisible(false);

    ui->statusbar->addPermanentWidget(m_jobStatusButton);
    ui->statusbar->addPermanentWidget(m_jobProgressBar);
    m_jobStatusButton->hide();
    m_jobProgressBar->hide();
}

void MainWindow::setupDriveWatcher()
{
    m_driveScanWatcher = new QFutureWatcher<QList<DriveInfo>>(this);
    connect(m_driveScanWatcher, &QFutureWatcher<QList<DriveInfo>>::finished,
            this,               &MainWindow::onDriveScanFinished);

    m_driveWatchTimer = new QTimer(this);
    m_driveWatchTimer->setInterval(kDriveWatchIntervalMs);

    // Network drives are not probed here — only when a dropdown is opened.
    connect(m_driveWatchTimer, &QTimer::timeout, this, [this]{
        requestDriveScan(/*probeNetwork=*/false);
    });

    m_driveWatchTimer->start();
    requestDriveScan(/*probeNetwork=*/false);   // initial fill of all dropdowns
}

void MainWindow::requestDriveScan(bool probeNetwork)
{
    if (m_driveScanWatcher->isRunning()) {
        // WM_DEVICECHANGE fires several times per plug-in, and a network probe
        // can take seconds — collapse everything into one follow-up scan.
        m_driveScanQueued      = true;
        m_driveScanQueuedProbe = m_driveScanQueuedProbe || probeNetwork;
        return;
    }

    m_driveScanWatcher->setFuture(QtConcurrent::run(&DriveScanner::scan, probeNetwork, m_knownShares));
}

void MainWindow::onSharesFound(const QStringList &uncPaths)
{
    bool changed = false;
    for (const QString &path : uncPaths) {
        if (!m_knownShares.contains(path, Qt::CaseInsensitive))
            m_knownShares << path;
        // Shown right away as Ready — the share list was just read from the server.
        changed |= DriveScanner::upsertNetworkShare(m_drives, path, DriveInfo::State::Ready);
    }

    if (changed)
        emit drivesChanged(m_drives);
}

void MainWindow::onDriveScanFinished()
{
    QList<DriveInfo> drives = m_driveScanWatcher->result();
    // A scan started before onSharesFound() does not know the newest shares —
    // add them back so they don't flicker out of the dropdown.
    for (const QString &share : std::as_const(m_knownShares))
        DriveScanner::upsertNetworkShare(drives, share, DriveInfo::State::Unknown);
    DriveScanner::carryOverNetworkState(drives, m_drives);

    const auto signatures = [](const QList<DriveInfo> &list) {
        QStringList result;
        for (const DriveInfo &drive : list)
            result << drive.signature();
        return result;
    };

    // Rebuilding every dropdown on each scan would reset an open popup for nothing.
    const bool changed = signatures(drives) != signatures(m_drives);
    m_drives = std::move(drives);
    if (changed)
        emit drivesChanged(m_drives);

    if (m_driveScanQueued) {
        const bool probe       = m_driveScanQueuedProbe;
        m_driveScanQueued      = false;
        m_driveScanQueuedProbe = false;
        requestDriveScan(probe);
    }
}

#ifdef _WIN32
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        const MSG *msg = static_cast<const MSG *>(message);
        // Sent when the taskbar button exists — again after an Explorer restart,
        // which drops the progress shown on the old button.
        static const UINT taskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
        if (msg->message == taskbarButtonCreated && m_taskbarProgress)
            m_taskbarProgress->reset();

        if (msg->message == WM_DEVICECHANGE &&
            (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE))
        {
            requestDriveScan(/*probeNetwork=*/false);
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::setupCore()
{
    m_worker = new FileWorkerThread(this);
    m_jobQueue = new JobQueueWidget(m_worker, this);

    m_taskbarProgress = new TaskbarProgress(this);
    connect(m_jobQueue,        &JobQueueWidget::taskbarProgressChanged,
            m_taskbarProgress, &TaskbarProgress::setProgress);

    // Created before the panels — every tab gets it in its constructor.
    m_folderSizeCache = new FolderSizeCache(this);

    // Created before the panels — connectPanel() wires every tab to it.
    m_networkDiscovery = new NetworkDiscovery(this);
    connect(m_networkDiscovery, &NetworkDiscovery::sharesFound,
            this,               &MainWindow::onSharesFound);
    connect(m_networkDiscovery, &NetworkDiscovery::runningChanged, this, [this](bool running){
        // Search done: re-probe so shares known from an earlier search that
        // did not answer this time turn grey.
        if (!running)
            requestDriveScan(/*probeNetwork=*/true);
    });
}

void MainWindow::setupWindow()
{
    resize(1200, 700);
    setWindowTitle(QStringLiteral("NexusCommander"));
}

void MainWindow::setupConnections()
{
    connect(m_worker, &FileWorkerThread::jobFinished,
            this,    &MainWindow::onJobFinished);
    connect(m_jobQueue, &JobQueueWidget::activityChanged,
            this,       &MainWindow::onJobActivityChanged);
    connect(m_jobQueue, &JobQueueWidget::batchFinished,
            this,       &MainWindow::onBatchFinished);
    connect(m_jobQueue, &JobQueueWidget::jobAdded,
            this,       &MainWindow::flashJobStatus);
    connect(m_jobQueue, &JobQueueWidget::pauseChanged,
            this,       &MainWindow::updatePauseQueueAction);

    connect(qApp, &QApplication::focusChanged,
            this, &MainWindow::onAppFocusChanged);
}

void MainWindow::setupUiComponents()
{
    createActions();
    setupPanels();
    setupToolBar();
    setupMenuBar();
    setupStatusBar();
}

QString MainWindow::findSourceContainingDest(const QStringList &sources, const QString &dest)
{
    for (const QString &path : sources) {
        // String check first — the stat runs on the GUI thread and a busy NAS answers after seconds.
        if (isSameOrInside(dest, path) && QFileInfo(path).isDir())
            return QDir::cleanPath(path);
    }
    return {};
}

void MainWindow::enqueueTransferJob(FileJob::Type type)
{
    FilePanel *sourcePanel      = activePanel();
    FilePanel *destinationPanel = inactivePanel();
    if (!sourcePanel || !destinationPanel)
        return;

    QStringList selected = sourcePanel->selectedPaths();
    if (selected.isEmpty())
        return;

    const QString dest   = QDir::cleanPath(destinationPanel->currentPath());
    const QString srcDir = QDir::cleanPath(sourcePanel->currentPath());

    if (pathEquals(srcDir, dest))  {
        const auto reply = QMessageBox::question(
            this,
            tr("Same folder"),
            tr("Source and destination are the same folder.\n"
               "Do you want to continue anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
            );

        if (reply == QMessageBox::No)
            return;
    }

    const QString verb = type == FileJob::Type::Copy ? tr("copied") : tr("moved");
    // Visible feedback that the job was queued: the cursor moves on, like Total Commander.
    if (validateAndEnqueue(type, std::move(selected), dest, verb))
        sourcePanel->advanceCursorPastSelection();
}

bool MainWindow::validateAndEnqueue(FileJob::Type type, QStringList sources,
                                    const QString &destination, const QString &operationVerb)
{
    if (sources.isEmpty() || destination.isEmpty())
        return false;

    warnAndRemoveActiveJobConflicts(sources, operationVerb);
    if (sources.isEmpty())
        return false;

    const QString cleanDest = QDir::cleanPath(destination);

    const QString conflictSource = findSourceContainingDest(sources, cleanDest);
    if (!conflictSource.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid destination"),
                             tr("Cannot copy/move \"%1\" into itself.\n"
                                "Destination \"%2\" is inside the source directory.")
                                 .arg(QFileInfo(conflictSource).fileName(),
                                      QDir::toNativeSeparators(cleanDest)));
        return false;
    }

    FileJob job;
    job.type        = type;
    job.sources     = std::move(sources);
    job.destination = cleanDest;
    // Fixed per job — toggling later does not change what a queued job copies.
    job.skipPartFiles = m_hidePartFiles;
    m_worker->enqueue(job);
    m_jobQueue->addJob(job);
    return true;
}

void MainWindow::warnAndRemoveActiveJobConflicts(QStringList &selected,
                                                 const QString &operationVerb)
{
    const QStringList activeSources = m_worker->activeSourcePaths();

    // Same path, a parent of an active source, or inside one.
    const auto conflictsWithActive = [&activeSources](const QString &candidate) {
        return std::any_of(activeSources.cbegin(), activeSources.cend(),
                           [&candidate](const QString &active) {
                               return isSameOrInside(active, candidate)
                                      || isSameOrInside(candidate, active);
                           });
    };

    QStringList conflictingNames;
    QStringList cleaned;
    cleaned.reserve(selected.size());

    for (const QString &path : std::as_const(selected)) {
        if (conflictsWithActive(path))
            conflictingNames << QFileInfo(path).fileName();
        else
            cleaned << path;
    }

    if (conflictingNames.isEmpty())
        return;

    QMessageBox::information(
        this,
        tr("Files skipped"),
        tr("The following items are being processed by an active job "
           "and will NOT be %1:\n\n%2")
            .arg(operationVerb, conflictingNames.join("\n")));

    selected = std::move(cleaned);
}

bool MainWindow::confirmDeletion(const QString &displayFolder, const QStringList &selected)
{
    QString text;
    if (selected.size() == 1) {
        text = tr("Delete \"%1\"?\n\nFrom: %2")
        .arg(QFileInfo(selected.first()).fileName(), displayFolder);
    } else {
        text = tr("Delete %1 items from:\n%2\n\n%3")
        .arg(selected.size())
            .arg(displayFolder, formatFileNamesText(selected));
    }

    QMessageBox dlg(QMessageBox::Question, tr("Delete"), text,
                    QMessageBox::Yes | QMessageBox::No, this);

    // forcing width via spacer
    if (auto *grid = qobject_cast<QGridLayout *>(dlg.layout()))
        grid->addItem(new QSpacerItem(210, 0),
                      grid->rowCount(), 0, 1, grid->columnCount());

    return dlg.exec() == QMessageBox::Yes;
}

bool MainWindow::handleTabKey(QObject *obj, QKeyEvent *event) {
    if (event->key() != Qt::Key_Tab || event->modifiers() != Qt::NoModifier)
        return false;

    auto *widget = qobject_cast<QWidget *>(obj);
    const bool isInsidePanel = widget
                               && (m_leftTabs->isAncestorOf(widget) || m_rightTabs->isAncestorOf(widget));

    if (!isInsidePanel)
        return false;

    if (auto *panel = qobject_cast<FilePanel *>(inactiveSide()->currentWidget()))
        panel->setFocus();
    return true;
}

bool MainWindow::handleMiddleClick(QObject *obj, QMouseEvent *event) {
    if (event->button() != Qt::MiddleButton)
        return false;

    auto *tabs = qobject_cast<QTabWidget *>(obj);
    if (!tabs)
        return false;

    QTabBar *tabBar = tabs->tabBar();
    const int tabIndex = tabBar->tabAt(tabBar->mapFrom(tabs, event->pos()));

    if (tabIndex < 0)
        return false;

    closeTab(tabs, tabIndex);
    return true;
}

void MainWindow::refreshAffectedTabs(const QStringList &affectedDirs)
{
    // Compared as strings, without touching the filesystem — canonicalPath()
    // stats every path on the GUI thread, which blocks on a slow or dead NAS
    // after every job. Panels and jobs both use cleaned absolute paths.
    QSet<QString> affected;
    for (const QString &dir : affectedDirs)
        affected.insert(pathKey(dir));

    for (FilePanel *panel : allPanels()) {
        if (affected.contains(pathKey(panel->currentPath())))
            panel->refresh();
    }
}

QList<FilePanel *> MainWindow::allPanels() const
{
    return panelsIn(m_leftTabs) + panelsIn(m_rightTabs);
}

QTabWidget *MainWindow::activeSide() const {
    QWidget *focused = focusWidget();
    const bool isRightSide = focused && m_rightTabs->isAncestorOf(focused);
    return isRightSide ? m_rightTabs : m_leftTabs;
}

QTabWidget *MainWindow::inactiveSide() const
{
    return activeSide() == m_leftTabs ? m_rightTabs : m_leftTabs;
}

FilePanel *MainWindow::activePanel() const
{
    QTabWidget *side = activeSide();
    return qobject_cast<FilePanel*>(side->currentWidget());
}

FilePanel *MainWindow::inactivePanel() const
{
    QTabWidget *side = inactiveSide();
    return qobject_cast<FilePanel*>(side->currentWidget());
}

void MainWindow::onAppFocusChanged(QWidget * /*old*/, QWidget *now)
{
    if (!now)
        return;

    FilePanel *focused = nullptr;
    for (QWidget *w = now; w; w = w->parentWidget()) {
        if (auto *p = qobject_cast<FilePanel *>(w)) {
            focused = p;
            break;
        }
    }

    if (!focused)
        return;

    for (FilePanel *panel : allPanels())
        panel->setActive(panel == focused);
}

void MainWindow::onStatusUpdate(const QString &message)
{
    m_statusLabel->setText(message);
}

void MainWindow::onCopyRequested()
{
    enqueueTransferJob(FileJob::Type::Copy);
}

void MainWindow::onMoveRequested()
{
    enqueueTransferJob(FileJob::Type::Move);
}

void MainWindow::onDeleteRequested()
{
    FilePanel *sourcePanel = activePanel();
    if (!sourcePanel) return;

    QStringList selected = sourcePanel->selectedPaths();
    if (selected.isEmpty()) return;

    warnAndRemoveActiveJobConflicts(selected, tr("deleted"));
    if (selected.isEmpty()) return;

    const QString folder = QDir::toNativeSeparators(sourcePanel->currentPath());
    if (!confirmDeletion(folder, selected))
        return;

    FileJob job;
    job.type    = FileJob::Type::Delete;
    job.sources = selected;

    m_worker->enqueue(job);
    m_jobQueue->addJob(job);
}

void MainWindow::onNewFolderRequested()
{
    FilePanel *panel = activePanel();
    if (panel)
        panel->createNewFolder();
}
void MainWindow::onRefreshRequested()
{
    for (FilePanel *panel : allPanels())
        panel->refresh();
}

void MainWindow::onToggleHiddenFiles(bool visible)
{
    m_showHiddenFiles = visible;
    for (FilePanel *panel : allPanels())
        panel->setShowHiddenFiles(visible);
}

void MainWindow::onToggleHidePartFiles(bool hide)
{
    m_hidePartFiles = hide;
    for (FilePanel *panel : allPanels())
        panel->setHidePartFiles(hide);
}

void MainWindow::onJobActivityChanged(int activeJobs, TaskbarProgress::State state,
                                      double fraction, const QString &remaining)
{
    const bool busy = activeJobs > 0;
    m_jobProgressBar->setVisible(busy);
    if (!busy) {
        // A finished batch keeps its outcome on the button for a moment (onBatchFinished()).
        if (!m_jobDoneTimer->isActive())
            m_jobStatusButton->hide();
        return;
    }
    m_jobDoneTimer->stop();
    m_jobStatusButton->show();

    const bool paused = state == TaskbarProgress::State::Paused;
    QString text = paused ? tr("%n job(s) paused", nullptr, activeJobs)
                          : tr("%n job(s) running", nullptr, activeJobs);
    if (!paused && !remaining.isEmpty())
        text += QStringLiteral("  —  ") + tr("%1 left").arg(remaining);
    if (state == TaskbarProgress::State::Error)
        text += QStringLiteral("  —  ") + tr("a job failed");

    m_jobStatusButton->setText(text);
    m_jobProgressBar->setValue(int(fraction * kJobProgressSteps));
}

void MainWindow::onBatchFinished(int doneJobs, bool failed)
{
    // Everything cancelled or skipped — nothing worth reporting.
    if (doneJobs == 0 && !failed)
        return;

    m_jobStatusButton->setText(failed ? tr("⚠ A job failed — see Job Queue")
                                      : tr("✓ %n job(s) done", nullptr, doneJobs));
    m_jobStatusButton->show();
    m_jobDoneTimer->start();
}

void MainWindow::flashJobStatus()
{
    m_jobStatusButton->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background-color: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  border-radius: 3px;"
        "}"));
    m_jobFlashTimer->start();
}

void MainWindow::updatePauseQueueAction(bool paused)
{
    if (paused) {
        m_pauseQueueAction->setIcon(QIcon::fromTheme("media-playback-start",
                                                     style()->standardIcon(QStyle::SP_MediaPlay)));
        m_pauseQueueAction->setText(tr("Resume Queue"));
        m_pauseQueueAction->setToolTip(tr("Queue paused — click to resume all jobs"));
    } else {
        m_pauseQueueAction->setIcon(QIcon::fromTheme("media-playback-pause",
                                                     style()->standardIcon(QStyle::SP_MediaPause)));
        m_pauseQueueAction->setText(tr("Pause Queue"));
        m_pauseQueueAction->setToolTip(tr("Pause all running and queued jobs"));
    }
}

void MainWindow::onPasteRequested(FilePanel *target)
{
    const QMimeData *clipboardData = QApplication::clipboard()->mimeData();

    if (!clipboardData || !clipboardData->hasUrls())
        return;

    const QString cleanDest = QDir::cleanPath(target->currentPath());

    // Filter clipboard URLs in one pass: keep only local files whose parent
    // directory differs from the destination (mixed-source clipboards from
    // other file managers may contain items from several folders).
    QStringList sources;
    int sameFolderCount = 0;
    for (const QUrl &url : clipboardData->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString filePath = url.toLocalFile();
        const QString parent   = QDir::cleanPath(QFileInfo(filePath).absolutePath());
        if (pathEquals(parent, cleanDest)) {
            ++sameFolderCount;
            continue;
        }
        sources << filePath;
    }

    if (sources.isEmpty()) {
        if (sameFolderCount > 0)
            QMessageBox::information(this, tr("Same folder"),
                                     tr("These files are already in this folder."));
        return;
    }

    const bool isCut          = ClipboardUtils::hasCutFlag(clipboardData);
    const FileJob::Type type  = isCut ? FileJob::Type::Move : FileJob::Type::Copy;
    const QString       verb  = isCut ? tr("moved") : tr("copied");

    if (!validateAndEnqueue(type, std::move(sources), target->currentPath(), verb))
        return;

    if (isCut)
        QApplication::clipboard()->clear();
}

void MainWindow::onJobFinished(QUuid jobId)
{
    const QStringList affected = m_jobQueue->affectedDirsForJob(jobId);

    if (affected.isEmpty()) {
        return;
    }

    // Also folders no tab is showing — their cached sizes (and their ancestors') are stale.
    for (const QString &dir : affected)
        m_folderSizeCache->invalidate(dir);

    refreshAffectedTabs(affected);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    const int activeJobs = m_jobQueue->activeJobCount();
    if (activeJobs > 0) {
        const auto reply = QMessageBox::warning(
            this,
            tr("Operations running"),
            tr("%n file operation(s) still running or queued.\n\n"
               "Quit anyway? They will be cancelled — the file being copied "
               "right now is deleted, files already copied completely are kept.",
               nullptr, activeJobs),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (reply == QMessageBox::No) {
            event->ignore();
            return;
        }
    }

    saveSettings();

    // ~FileWorkerThread cancels the jobs and waits — the copy guard then
    // removes the partially written file.
    event->accept();
}

QString MainWindow::settingsFilePath()
{
    // Per user, always writable — next to the exe fails in Program Files and is
    // shared by all users. Window place on a missing monitor and missing folders
    // are checked on restore, so a roaming profile on another PC is fine.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/NexusCommander.ini");
}

void MainWindow::saveSettings() const
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("Window"));
    // Closed while maximized or minimized: the size to come back to is the normal one.
    const bool maximized = windowState() & Qt::WindowMaximized;
    const bool normal    = !(windowState() & (Qt::WindowMaximized | Qt::WindowMinimized));
    settings.setValue(QStringLiteral("Geometry"), normal ? geometry() : normalGeometry());
    settings.setValue(QStringLiteral("Maximized"), maximized);
    settings.setValue(QStringLiteral("Screen"), screen()->name());
    QStringList splitterSizes;
    for (const int size : m_splitter->sizes())
        splitterSizes << QString::number(size);
    settings.setValue(QStringLiteral("Splitter"), splitterSizes);
    settings.setValue(QStringLiteral("ActiveSide"),
                      activeSide() == m_rightTabs ? QStringLiteral("right") : QStringLiteral("left"));
    settings.endGroup();

    settings.beginGroup(QStringLiteral("View"));
    settings.setValue(QStringLiteral("ShowHiddenFiles"), m_showHiddenFiles);
    settings.setValue(QStringLiteral("HidePartFiles"), m_hidePartFiles);
    settings.endGroup();

    saveTabs(settings, QStringLiteral("LeftPanel"),  m_leftTabs);
    saveTabs(settings, QStringLiteral("RightPanel"), m_rightTabs);
}

void MainWindow::saveTabs(QSettings &settings, const QString &group, const QTabWidget *side)
{
    // Removed first — otherwise entries of closed tabs would stay in the file.
    settings.remove(group);
    settings.beginGroup(group);
    settings.setValue(QStringLiteral("CurrentTab"), side->currentIndex());

    const QList<FilePanel *> panels = panelsIn(side);
    settings.beginWriteArray(QStringLiteral("Tabs"), int(panels.size()));
    for (int i = 0; i < panels.size(); ++i) {
        settings.setArrayIndex(i);
        panels.at(i)->saveSettings(settings);
    }
    settings.endArray();
    settings.endGroup();
}

void MainWindow::restoreSettings()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);

    // Before the tabs — addTab() gives every new tab the current view settings.
    m_showHiddenAction->setChecked(
        settings.value(QStringLiteral("View/ShowHiddenFiles"), m_showHiddenFiles).toBool());
    m_hidePartFilesAction->setChecked(
        settings.value(QStringLiteral("View/HidePartFiles"), m_hidePartFiles).toBool());

    restoreTabs(settings, QStringLiteral("LeftPanel"),  m_leftTabs);
    restoreTabs(settings, QStringLiteral("RightPanel"), m_rightTabs);

    // Window size first — the splitter divides it.
    restoreWindowGeometry(settings);

    QList<int> splitterSizes;
    const QStringList savedSizes = settings.value(QStringLiteral("Window/Splitter")).toStringList();
    for (const QString &size : savedSizes)
        splitterSizes << size.toInt();
    if (splitterSizes.size() == 2 && splitterSizes.at(0) > 0 && splitterSizes.at(1) > 0)
        m_splitter->setSizes(splitterSizes);

    QTabWidget *side = settings.value(QStringLiteral("Window/ActiveSide")).toString() == QLatin1String("right")
                           ? m_rightTabs : m_leftTabs;
    side->currentWidget()->setFocus();
}

void MainWindow::restoreTabs(QSettings &settings, const QString &group, QTabWidget *side)
{
    settings.beginGroup(group);
    const int count = settings.beginReadArray(QStringLiteral("Tabs"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        // setupPanels() created one start tab per side — it becomes the first saved tab.
        auto *panel = i == 0 ? qobject_cast<FilePanel *>(side->widget(0)) : addTab(side);
        if (panel)
            panel->restoreSettings(settings);
    }
    settings.endArray();

    const int current = settings.value(QStringLiteral("CurrentTab"), 0).toInt();
    if (current >= 0 && current < side->count())
        side->setCurrentIndex(current);
    settings.endGroup();
}

void MainWindow::restoreWindowGeometry(const QSettings &settings)
{
    const QRect   saved      = settings.value(QStringLiteral("Window/Geometry")).toRect();
    const QString screenName = settings.value(QStringLiteral("Window/Screen")).toString();
    if (!saved.isValid())
        return;   // first start — default size, Windows picks the place

    // Back on the monitor the window was closed on — only if it is still connected
    // and the place is still on it (resolution or arrangement may have changed).
    QScreen *target = nullptr;
    for (QScreen *screen : QGuiApplication::screens()) {
        const QRect visible = screen->availableGeometry().intersected(saved);
        if (screen->name() == screenName
            && visible.width() >= kMinVisibleWidth && visible.height() >= kMinVisibleHeight) {
            target = screen;
            break;
        }
    }

    if (target) {
        setGeometry(saved);
    } else {
        // Monitor gone: same size, centred on the primary monitor.
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        QRect rect(QPoint(), saved.size().boundedTo(available.size()));
        rect.moveCenter(available.center());
        setGeometry(rect);
    }

    // show() in main() keeps the state — maximized on the monitor placed above.
    if (settings.value(QStringLiteral("Window/Maximized")).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
        if (handleTabKey(obj, static_cast<QKeyEvent *>(event)))
            return true;
        break;

    case QEvent::MouseButtonPress: {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (handleMiddleClick(obj, mouseEvent))
            return true;
        if (handleThumbButtons(mouseEvent))
            return true;
        break;
    }

    default:
        break;
    }

    return QMainWindow::eventFilter(obj, event);
}

bool MainWindow::handleThumbButtons(QMouseEvent *event)
{
    FilePanel *panel = activePanel();
    if (!panel)
        return false;

    switch (event->button()) {
    case Qt::XButton1: panel->onGoBack();    return true;
    case Qt::XButton2: panel->onGoForward(); return true;
    default:    return false;
    }
}