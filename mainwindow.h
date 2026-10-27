#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QTabWidget>
#include <QLabel>
#include <QProgressBar>
#include <QToolButton>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QFutureWatcher>
#include "driveScanner.h"
#include "filePanel.h"
#include "folderSizeCache.h"
#include "networkDiscovery.h"
#include "jobQueueWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

/**
 * @brief Top-level window — owns two QTabWidgets (left/right), each containing
 *        one or more FilePanel tabs. Also owns the FileWorkerThread and JobQueueWidget.
 *
 * Provides a toolbar, menu bar, and F-key shortcuts for the core operations
 * (F2 Refresh, F5 Copy, F6 Move, F7 New Folder, F8 Delete). Copy/Move always
 * go from the active (focused) panel to the inactive panel. Clipboard paste
 * (Ctrl+V) targets whichever panel emitted the signal.
 */

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    /// @brief Emitted when a background drive scan found a different drive set or state.
    void drivesChanged(const QList<DriveInfo> &drives);

protected:
    /// @brief Handle MouseThumbButtons and Tab-Key
    bool eventFilter(QObject *obj, QEvent *event) override;
    /// @brief Warn before quitting while jobs are running or queued.
    void closeEvent(QCloseEvent *event) override;
#ifdef _WIN32
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private slots:
    /// @brief Single qApp-wide focus listener — replaces the per-panel hook.
    /// Iterates all panels and calls setActive() on each.
    void onAppFocusChanged(QWidget *old, QWidget *now);
    /// @brief Forward panel status text to the window status bar.
    void onStatusUpdate(const QString &message);
    /// @brief F5 handler — enqueue copy from active to inactive panel.
    void onCopyRequested();
    /// @brief F6 handler — enqueue move from active to inactive panel.
    void onMoveRequested();
    /// @brief F8 handler — delete selected items in the active panel.
    void onDeleteRequested();
    /// @brief F7 handler — create a new folder in the active panel.
    void onNewFolderRequested();
    /// @brief F2 handler — refresh all tabs on both sides.
    void onRefreshRequested();
    /// @brief Toggle hidden/system file visibility on all panels.
    void onToggleHiddenFiles(bool visible);
    /// @brief Hide .part files on all panels — copy / move enqueued from now on leave them out.
    void onToggleHidePartFiles(bool hide);
    /// @brief Show / update / hide the job indicator in the status bar.
    void onJobActivityChanged(int activeJobs, TaskbarProgress::State state, double fraction,
                              const QString &remaining);
    /// @brief The queue went idle — the status-bar button shows the outcome for a few seconds.
    void onBatchFinished(int doneJobs, bool failed);
    /// @brief Briefly highlight the status-bar button — feedback that F5 / F6 / F8 queued a job.
    void flashJobStatus();
    /// @brief Switch the Pause / Resume Queue action's icon and text to @p paused.
    void updatePauseQueueAction(bool paused);

    /// @brief Handle Ctrl+V — detects cut vs. copy via Windows "Preferred DropEffect".
    void onPasteRequested(FilePanel *target);

    void onJobFinished(QUuid jobId);
    /// @brief Publish a finished drive scan to all panels (only if something changed).
    void onDriveScanFinished();
    /// @brief Add shares found by NetworkDiscovery to the drive list (kept for the session).
    void onSharesFound(const QStringList &uncPaths);

private:
    Ui::MainWindow *ui;

    // --- Widgets ---
    QTabWidget     *m_leftTabs    = nullptr;
    QTabWidget     *m_rightTabs   = nullptr;
    QSplitter      *m_splitter    = nullptr;
    QLabel         *m_statusLabel = nullptr;
    // Job indicator on the right of the status bar — visible only while jobs run.
    QToolButton    *m_jobStatusButton = nullptr;   ///< "2 jobs running — 1:23 left", opens the Job Queue.
    QProgressBar   *m_jobProgressBar  = nullptr;   ///< Batch progress, like the taskbar button.
    QTimer         *m_jobFlashTimer   = nullptr;   ///< Ends the highlight after a job was queued.
    QTimer         *m_jobDoneTimer    = nullptr;   ///< Hides the "✓ N job(s) done" summary.

    // View settings for all tabs, also applied to new ones — per session, never saved.
    bool            m_showHiddenFiles = false;
    bool            m_hidePartFiles   = true;    ///< On by default.

    // --- Background worker + job queue dialog ---
    FileWorkerThread     *m_worker   = nullptr;
    JobQueueWidget *m_jobQueue = nullptr;
    TaskbarProgress      *m_taskbarProgress = nullptr;   ///< Job progress on the taskbar button.

    // Drive list is scanned off the GUI thread (DriveScanner::scan). Triggers:
    // WM_DEVICECHANGE + slow fallback timer (no network probe) and opening a
    // drive dropdown (with network probe). Panels get the result via drivesChanged().
    QTimer                          *m_driveWatchTimer   = nullptr;
    QFutureWatcher<QList<DriveInfo>> *m_driveScanWatcher = nullptr;
    QList<DriveInfo>                 m_drives;              ///< Last published scan result.
    bool                             m_driveScanQueued      = false; ///< Rescan requested while one was running.
    bool                             m_driveScanQueuedProbe = false; ///< Queued rescan should probe network drives.

    // LAN search ("Find network drives…" in the dropdown). Found shares are
    // remembered for the session only — never saved — and passed to every
    // scan, so a share that disappears later shows up greyed out.
    NetworkDiscovery *m_networkDiscovery = nullptr;
    QStringList       m_knownShares;

    // Folder sizes calculated this session, shared by all tabs — memory only, never saved.
    FolderSizeCache  *m_folderSizeCache  = nullptr;

    // Actions
    QAction *m_newTabAction    = nullptr;
    QAction *m_closeTabAction  = nullptr;
    QAction *m_copyAction      = nullptr;
    QAction *m_moveAction      = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_deleteAction    = nullptr;
    QAction *m_refreshAction   = nullptr;
    QAction *m_jobQueueAction  = nullptr;
    QAction *m_pauseQueueAction = nullptr;   ///< Pause / resume the whole queue — icon follows the state.
    QAction *m_hidePartFilesAction = nullptr;

    /// @return The panel (or child of) that currently has keyboard focus.
    FilePanel *activePanel()   const;
    /// @return The other panel — destination for copy/move (current tab of opposite side).
    FilePanel *inactivePanel() const;

    /// @return The QTabWidget that contains the focused panel.
    QTabWidget *activeSide() const;
    /// @return The QTabWidget on the opposite side.
    QTabWidget *inactiveSide() const;

    /// @return Every FilePanel tab on both sides.
    QList<FilePanel *> allPanels() const;

    /// @brief Create a new FilePanel tab in @p side, navigated to @p path.
    FilePanel *addTab(QTabWidget *side, const QString &path = QString());
    /// @brief Close tab at @p index in @p side (minimum 1 tab enforced).
    void closeTab(QTabWidget *side, int index);
    /// @brief Connect all signals for a newly created FilePanel.
    void connectPanel(FilePanel *panel, QTabWidget *side);
    void createActions();
    void initTabWidget(QTabWidget *tabs);
    void setupPanels();
    void setupToolBar();
    void setupMenuBar();
    void setupStatusBar();
    void setupDriveWatcher();
    /// @brief Start a background drive scan; bursts are collapsed into one follow-up scan.
    void requestDriveScan(bool probeNetwork);
    void setupCore();
    void setupWindow();
    void setupConnections();
    void setupUiComponents();

    /// @brief Return the source path that contains @p dest, or empty if none.
    static QString findSourceContainingDest(const QStringList &sources, const QString &dest);
    /// @brief Enqueue a single transfer job (Copy or Move) covering all selected sources.
    void enqueueTransferJob(FileJob::Type type);
    /// @brief Shared validation + enqueue path used by F5/F6 and Ctrl+V.
    /// Runs the active-job-conflict filter and the "destination inside source" check,
    /// then enqueues the job. Returns false if the job was rejected or cleaned down to nothing.
    bool validateAndEnqueue(FileJob::Type type, QStringList sources,
                            const QString &destination, const QString &operationVerb);
    /// @brief Remove items from @p selected that conflict with running jobs and warn the user.
    void warnAndRemoveActiveJobConflicts(QStringList &selected, const QString &operationVerb);
    /// @brief Show a confirmation dialog for the delete operation.
    bool confirmDeletion(const QString &displayFolder, const QStringList &selected);
    // eventFilter() helpers — return true if the event was handled
    bool handleTabKey(QObject *obj, QKeyEvent *event);
    bool handleThumbButtons(QMouseEvent *event);
    bool handleMiddleClick(QObject *obj, QMouseEvent *event);
    /// @brief Refresh every tab that shows one of @p affectedDirs.
    void refreshAffectedTabs(const QStringList &affectedDirs);
};
