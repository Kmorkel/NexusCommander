#include "mainwindow.h"
#include "./ui_mainwindow.h"

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
#include <QGridLayout>
#include <QSpacerItem>
#include "clipboardUtils.h"

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

/// @brief Path comparison casing — Windows is case-insensitive, POSIX is case-sensitive.
constexpr Qt::CaseSensitivity kPathCase =
#ifdef Q_OS_WIN
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

bool pathEquals(const QString &a, const QString &b)
{
    return QString::compare(a, b, kPathCase) == 0;
}

bool pathStartsWith(const QString &path, const QString &prefix)
{
    return path.startsWith(prefix, kPathCase);
}

/// @brief Build a display string for confirmation dialogs (up to @p maxShown names).
QString formatFileNamesText(const QStringList &paths, int maxShown = 5)
{
    QStringList names;
    for (const QString &filePath : paths)
        names << QFileInfo(filePath).fileName();

    QString text = names.mid(0, maxShown).join("\n");
    if (paths.size() > maxShown)
        text += QString("\n… and %1 more").arg(paths.size() - maxShown);
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
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupPanels()
{
    m_leftTabs  = new QTabWidget(this);
    m_rightTabs = new QTabWidget(this);

    initTabWidget(m_leftTabs);
    initTabWidget(m_rightTabs);

    addTab(m_leftTabs,  QDir::homePath());
    addTab(m_rightTabs, QDir::homePath());

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_leftTabs);
    m_splitter->addWidget(m_rightTabs);
    m_splitter->setSizes({600, 600});

    setCentralWidget(m_splitter);
}

void MainWindow::connectPanel(FilePanel *panel, QTabWidget *side)
{
    connect(panel, &FilePanel::statusMessage, this, &MainWindow::onStatusUpdate);
    connect(panel, &FilePanel::deleteRequested, this, &MainWindow::onDeleteRequested);
    connect(panel, &FilePanel::pasteRequested, this, [this, panel]{ onPasteRequested(panel); });

    connect(panel, &FilePanel::pathChanged, this, [side, panel](const QString &path){
        const int idx = side->indexOf(panel);
        if (idx < 0) return;

        QString dirName = QDir(path).dirName();
        if (dirName.isEmpty()) dirName = path;
        side->setTabText(idx, dirName);
        side->setTabToolTip(idx, QDir::toNativeSeparators(path));
    });

    connect(panel, &FilePanel::openInNewTabRequested, this, [this, side](const QString &path){
        addTab(side, path);
    });

    // The shared drive watcher in MainWindow tells every panel when mounted
    // volumes change — panels never run their own QStorageInfo poll.
    connect(this, &MainWindow::driveTopologyChanged,
            panel, &FilePanel::onDriveTopologyChanged);
}

void MainWindow::createActions()
{
    m_newTabAction = new QAction("New &Tab", this);
    m_newTabAction->setShortcut(Qt::CTRL | Qt::Key_T);
    connect(m_newTabAction, &QAction::triggered, this, [this]{
        QTabWidget *side = activeSide();
        FilePanel *panel = addTab(side);
        side->setCurrentWidget(panel);
        panel->setFocus();
    });

    m_closeTabAction = new QAction("&Close Tab", this);
    m_closeTabAction->setShortcut(Qt::CTRL | Qt::Key_W);
    connect(m_closeTabAction, &QAction::triggered, this, [this]{
        QTabWidget *side = activeSide();
        closeTab(side, side->currentIndex());
    });

    m_copyAction = new QAction(QIcon::fromTheme("document-copy"), "F5 Copy", this);
    m_copyAction->setShortcut(Qt::Key_F5);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::onCopyRequested);

    m_moveAction = new QAction(QIcon::fromTheme("go-jump"), "F6 Move", this);
    m_moveAction->setShortcut(Qt::Key_F6);
    connect(m_moveAction, &QAction::triggered, this, &MainWindow::onMoveRequested);

    m_newFolderAction = new QAction(QIcon::fromTheme("folder-new"), "F7 New Folder", this);
    m_newFolderAction->setShortcut(Qt::Key_F7);
    connect(m_newFolderAction, &QAction::triggered, this, &MainWindow::onNewFolderRequested);

    m_deleteAction = new QAction(QIcon::fromTheme("edit-delete"), "F8 Delete", this);
    m_deleteAction->setShortcut(Qt::Key_F8);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::onDeleteRequested);

    m_refreshAction = new QAction(QIcon::fromTheme("view-refresh"), "F2 Refresh", this);
    m_refreshAction->setShortcut(Qt::Key_F2);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::onRefreshRequested);

    m_jobQueueAction = new QAction(QIcon::fromTheme("system-run"), "Job Queue", this);
    connect(m_jobQueueAction, &QAction::triggered, this, [this]{
        m_jobQueue->show();
        m_jobQueue->raise();
    });
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

    auto *newTabBtn = new QToolButton(tabs);
    newTabBtn->setText("+");
    newTabBtn->setToolTip("New Tab (Ctrl+T)");
    newTabBtn->setAutoRaise(true);
    tabs->setCornerWidget(newTabBtn, Qt::TopRightCorner);

    connect(newTabBtn, &QToolButton::clicked, this, [this, tabs]{
        addTab(tabs);
    });

    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index){
        closeTab(tabs, index);
    });
}

FilePanel *MainWindow::addTab(QTabWidget *side, const QString &path)
{
    QString startPath = path;
    if (startPath.isEmpty())
        startPath = QDir::homePath();

    auto *panel = new FilePanel(startPath, side);
    connectPanel(panel, side);

    QString label = QDir(startPath).dirName();
    if (label.isEmpty())
        label = startPath;   // root dirs like "C:/" have no dirName

    const int idx = side->addTab(panel, label);
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
    QToolBar *bar = addToolBar("Main");
    bar->setMovable(false);

    bar->addAction(m_copyAction);
    bar->addAction(m_moveAction);
    bar->addAction(m_newFolderAction);
    bar->addAction(m_deleteAction);
    bar->addSeparator();
    bar->addAction(m_refreshAction);
    bar->addSeparator();
    bar->addAction(m_jobQueueAction);
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = ui->menubar->addMenu("&File");
    fileMenu->addAction(m_newTabAction);
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_newFolderAction);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit\tAlt+F4", qApp, &QApplication::quit);

    QMenu *viewMenu = ui->menubar->addMenu("&View");
    viewMenu->addAction(m_refreshAction);

    QAction *hiddenAct = viewMenu->addAction("Show &Hidden Files");
    hiddenAct->setCheckable(true);
    connect(hiddenAct, &QAction::toggled,
            this, &MainWindow::onToggleHiddenFiles);

    viewMenu->addSeparator();
    viewMenu->addAction(m_jobQueueAction);

    QMenu *helpMenu = ui->menubar->addMenu("&Help");
    helpMenu->addAction("&About", this, [this]{
        QMessageBox::about(this, "About NexusCommander",
                           "<h2>NexusCommander</h2>"
                           "<p>Version 1.0.0</p>"
                           "<p>A fast, dual-pane file manager for Windows,<br>"
                           "inspired by Total Commander.</p>"
                           "<p>Licensed under the "
                           R"(<a href="https://github.com/Kmorkel/NexusCommander/blob/main/LICENSE">MIT License</a>)"
                           "<p><b>Shortcuts:</b><br>"
                           "F2 Refresh<br>"
                           "F5 Copy &nbsp; F6 Move<br>"
                           "F7 New Folder &nbsp; F8 Delete<br>"
                           "Ctrl+T New Tab &nbsp; Ctrl+W Close Tab</p>"
                           "<hr>"
                           "<p><b>Third-Party Software</b></p>"
                           "<p>This application uses the <a href=\"https://www.qt.io/\">Qt Framework</a> "
                           "(version " QT_VERSION_STR "), licensed under the "
                           "<a href=\"https://www.gnu.org/licenses/lgpl-3.0.html\">LGPL v3</a>. "
                           "Qt is linked dynamically; no modifications were made to the Qt source code.</p>"
                           "<p>Qt source code is available at "
                           "<a href=\"https://www.qt.io/download-open-source\">qt.io/download-open-source</a>.</p>");
    });
}

void MainWindow::setupStatusBar()
{
    m_statusLabel = new QLabel("", this);
    ui->statusbar->addWidget(m_statusLabel, 1);
}

void MainWindow::setupDriveWatcher()
{
    m_driveWatchTimer = new QTimer(this);
    m_driveWatchTimer->setInterval(kDriveWatchIntervalMs);

    connect(m_driveWatchTimer, &QTimer::timeout,
            this,              &MainWindow::pollMountedDrives);

    m_driveWatchTimer->start();
}

#ifdef _WIN32
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_DEVICECHANGE &&
            (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE))
        {
            pollMountedDrives();
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::setupCore()
{
    m_worker = new FileWorkerThread(this);
    m_jobQueue = new JobQueueWidget(m_worker, this);
}

void MainWindow::setupWindow()
{
    resize(1200, 700);
    setWindowTitle("NexusCommander");
}

void MainWindow::setupConnections()
{
    connect(m_worker, &FileWorkerThread::jobFinished,
            this,    &MainWindow::onJobFinished);

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
    const QString cleanDest = QDir::cleanPath(dest);

    for (const QString &path : sources) {
        if (!QFileInfo(path).isDir())
            continue;

        const QString cleanSrc = QDir::cleanPath(path);

        if (pathEquals(cleanDest, cleanSrc))
            return cleanSrc;

        // QDir::relativeFilePath normalizes separators and trailing slashes.
        // If `dest` is outside `src`, the result starts with ".." (or is absolute).
        // If it’s inside, it won’t start with ".." → valid match.
        const QString rel = QDir(cleanSrc).relativeFilePath(cleanDest);
        if (!rel.isEmpty() && !rel.startsWith(QLatin1String(".."))
            && !QDir::isAbsolutePath(rel)) {
            return cleanSrc;
        }
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
    validateAndEnqueue(type, std::move(selected), dest, verb);
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
    m_worker->enqueue(job);
    m_jobQueue->addJob(job);
    return true;
}

void MainWindow::warnAndRemoveActiveJobConflicts(QStringList &selected,
                                                 const QString &operationVerb)
{
    const QStringList activeList = m_worker->activeSourcePaths();

    QVector<QString> activeNorm;
    activeNorm.reserve(activeList.size());

    for (const QString &p : activeList)
        activeNorm.append( QDir::cleanPath(p));

    const auto conflictsWithActive = [&](const QString &candidate) {
        const QString normalized =  QDir::cleanPath(candidate);

        for (const QString &active : activeNorm) {
            if (pathEquals(active, normalized))
                return true;                          // same path
            if (pathStartsWith(active, normalized + '/'))
                return true;                          // candidate is parent of an active source
            if (pathStartsWith(normalized, active + '/'))
                return true;                          // candidate is inside an active source
        }
        return false;
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

void MainWindow::refreshAffectedTabs(QTabWidget *side, const QStringList &affectedDirs)
{
    // canonicalPath() ensures that paths pointing to the same folder are
    // recognised as equal even if they differ in case, symlinks/junctions
    // or path style. Resolved once into a QSet (which also dedups) so the
    // loop below stays at O(N+M) filesystem calls instead of O(N*M).
    QSet<QString> canonicalAffected;
    for (const QString &dir : affectedDirs) {
        const QString c = QDir(dir).canonicalPath();
        if (!c.isEmpty())
            canonicalAffected.insert(c);
    }

    for (int i = 0; i < side->count(); ++i) {
        auto *panel = qobject_cast<FilePanel*>(side->widget(i));
        if (!panel) continue;

        const QString panelCanon = QDir(panel->currentPath()).canonicalPath();
        if (canonicalAffected.contains(panelCanon))
            panel->refresh();
    }
}

QTabWidget *MainWindow::activeSide() const {
    QWidget *focused = focusWidget();
    const bool isRightSide = focused && m_rightTabs->isAncestorOf(focused);
    return isRightSide ? m_rightTabs : m_leftTabs;
}

QTabWidget *MainWindow::inactiveSide() const
{
    if (activeSide() == m_leftTabs)
        return m_rightTabs;
    else
        return m_leftTabs;
}

void MainWindow::refreshFilePanels(QTabWidget *tabs)
{
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *panel = qobject_cast<FilePanel*>(tabs->widget(i)))
            panel->refresh();
    }
}

void MainWindow::setHiddenFilesVisibility(QTabWidget *tabs, bool visible)
{
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *panel = qobject_cast<FilePanel*>(tabs->widget(i)))
            panel->setShowHiddenFiles(visible);
    }
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

    const auto notify = [focused](QTabWidget *tabs) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *panel = qobject_cast<FilePanel *>(tabs->widget(i)))
                panel->setActive(panel == focused);
        }
    };
    notify(m_leftTabs);
    notify(m_rightTabs);
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
    refreshFilePanels(m_leftTabs);
    refreshFilePanels(m_rightTabs);
}

void MainWindow::onToggleHiddenFiles(bool visible)
{
    setHiddenFilesVisibility(m_leftTabs, visible);
    setHiddenFilesVisibility(m_rightTabs, visible);
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

void MainWindow::onJobFinished(QUuid jobId, FileJob::Status status, const QString &message)
{
    const QStringList affected = m_jobQueue->affectedDirsForJob(jobId);

    if (affected.isEmpty()) {
        return;
    }

    refreshAffectedTabs(m_leftTabs,  affected);
    refreshAffectedTabs(m_rightTabs, affected);
}

void MainWindow::pollMountedDrives()
{
    // Combine root path + volume label into a single signature so renaming
    // a drive (without remount) also fires driveTopologyChanged().
    QStringList currentSignatures;

    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (volume.isReady())
            currentSignatures << volume.rootPath() + QLatin1Char('|') + volume.name();
    }

    if (currentSignatures != m_lastDriveRoots) {
        m_lastDriveRoots = std::move(currentSignatures);
        emit driveTopologyChanged();
    }
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