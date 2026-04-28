#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
#include <QToolBar>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QStorageInfo>
#include "filePanel.h"
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
    /// @brief Emitted by the shared drive-watch timer when mounted volumes change.
    void driveTopologyChanged();

protected:
    /// @brief Handle MouseThumbButtons and Tab-Key
    bool eventFilter(QObject *obj, QEvent *event) override;
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

    /// @brief Handle Ctrl+V — detects cut vs. copy via Windows "Preferred DropEffect".
    void onPasteRequested(FilePanel *target);

    void onJobFinished(QUuid jobId, FileJob::Status status, const QString &message);
    void pollMountedDrives();

private:
    Ui::MainWindow *ui;

    // --- Widgets ---
    QTabWidget     *m_leftTabs    = nullptr;
    QTabWidget     *m_rightTabs   = nullptr;
    QSplitter      *m_splitter    = nullptr;
    QLabel         *m_statusLabel = nullptr;

    // --- Background worker + job queue dialog ---
    FileWorkerThread     *m_worker   = nullptr;
    JobQueueWidget *m_jobQueue = nullptr;

    // Single shared timer that polls QStorageInfo every 2 s. Each FilePanel
    // listens to driveTopologyChanged() and rebuilds its drive bar locally.
    QTimer      *m_driveWatchTimer = nullptr;
    QStringList  m_lastDriveRoots;

    // Actions
    QAction *m_newTabAction    = nullptr;
    QAction *m_closeTabAction  = nullptr;
    QAction *m_copyAction      = nullptr;
    QAction *m_moveAction      = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_deleteAction    = nullptr;
    QAction *m_refreshAction   = nullptr;
    QAction *m_jobQueueAction  = nullptr;

    /// @return The panel (or child of) that currently has keyboard focus.
    FilePanel *activePanel()   const;
    /// @return The other panel — destination for copy/move (current tab of opposite side).
    FilePanel *inactivePanel() const;

    /// @return The QTabWidget that contains the focused panel.
    QTabWidget *activeSide() const;
    /// @return The QTabWidget on the opposite side.
    QTabWidget *inactiveSide() const;

    /// @brief Refresh every FilePanel tab inside @p tabs.
    void refreshFilePanels(QTabWidget *tabs);
    /// @brief Set hidden-file visibility on every FilePanel in @p tabs.
    void setHiddenFilesVisibility(QTabWidget *tabs, bool visible);

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
    void refreshAffectedTabs(QTabWidget *side, const QStringList &affectedDirs);
};
