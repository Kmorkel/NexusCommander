#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QFileSystemModel>
#include <QFrame>
#include <QFutureWatcher>
#include <QTreeView>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QStack>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QHBoxLayout>
#include <QVector>
#include <QTimer>
#include <atomic>
#include <memory>
#include "driveScanner.h"
#include "folderSizeCache.h"

class QMenu;

/// Where folders go in the sorted file list — chosen per tab, never saved.
enum class FolderPlacement { Top, Bottom, Mixed };

/**
 * @brief One pane of the dual-panel file manager.
 *
 * Displays an address bar, back/forward navigation, a drive dropdown with
 * quick-access buttons for the user folders (Desktop, Documents, …), a file list
 * (QTreeView with Name / Size / Type / Date columns, backed by QFileSystemModel +
 * WildcardFilterProxy), and a filter bar.
 * Two FilePanel instances are placed side-by-side in a QSplitter by MainWindow.
 *
 * Folder sizes are only calculated on request (Space, Σ button, context menu, or
 * the info bar shown when sorting by size) — on a network drive a walk can take
 * minutes. Results go into MainWindow's FolderSizeCache, shared by all tabs.
 *
 * Navigation history is maintained via two stacks (back/forward), similar to a
 * web browser. Keyboard shortcuts (Ctrl+C/X/V, Del, Backspace, Ctrl+S for
 * filter, mouse thumb buttons) are scoped to the panel that has focus.
 */
class WildcardFilterProxy; // forward declaration

class FilePanel : public QWidget
{
    Q_OBJECT

public:
    /// @p sizeCache is MainWindow's session-wide folder size cache (must outlive the panel).
    FilePanel(const QString &rootPath, FolderSizeCache *sizeCache, QWidget *parent = nullptr);
    ~FilePanel() override;

    /// @return Absolute path of the directory currently shown.
    QString     currentPath() const;

    /// @return Absolute paths of all selected files/directories.
    QStringList selectedPaths() const;

    /// @brief After F5 / F6: clear the selection and put the cursor on the entry
    /// below the last selected one (stays on the last row at the end of the list).
    void advanceCursorPastSelection();

    /// @brief Navigate to @p path and record it in the back history. A missing
    /// path or a file is reported in a message box; the panel stays where it is.
    void navigateTo(const QString &path);

    /// @brief Strip surrounding whitespace and trailing dots (Windows normalization).
    static QString sanitizeFileName(const QString &name);

    /// @brief Validate a file/folder name for the current OS. Sets @p errorString on failure.
    static bool isValidEntryName(const QString &name, QString &errorString);

    /// @brief Force re-read of the current directory.
    void refresh();

    /// @brief Prompt the user and create a new folder in the current directory.
    void createNewFolder();

    /// @brief Show or hide hidden/system files.
    void setShowHiddenFiles(bool visible);

    /// @brief Hide ".part" files (unfinished downloads) in the list.
    void setHidePartFiles(bool hide);

    /// @brief Mark the panel as focused/unfocused. Called by MainWindow's
    /// central focus listener — never wire this to qApp::focusChanged
    /// from inside the panel (would scale O(N) with the number of tabs).
    void setActive(bool active);

signals:
    /// @brief Status bar text for MainWindow (e.g. "42 items | 3 selected | C:/…").
    void statusMessage(const QString &msg);

    /// @brief Emitted whenever the displayed directory changes.
    void pathChanged(const QString &path);

    /// @brief User chose "Open in New Tab" from context menu on a directory.
    void openInNewTabRequested(const QString &path);

    void deleteRequested();     ///< User pressed Delete.
    void pasteRequested();      ///< User pressed Ctrl+V.

    /// @brief User is opening the drive dropdown — MainWindow re-scans and
    /// probes network drives (the only time they are probed).
    void driveScanRequested();

    /// @brief User picked "Find network drives…" in the drive dropdown.
    void networkSearchRequested();

    /// @brief Shares listed in the "Connect to server" dialog — MainWindow keeps
    /// them in the dropdown for the session, like search results.
    void networkSharesFound(const QStringList &uncPaths);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

public slots:
    void onGoBack();
    void onGoForward();

    /// @brief Rebuild the drive dropdown from MainWindow's background scan result.
    void setDrives(const QList<DriveInfo> &drives);

    /// @brief Switch the dropdown's search entry between "Find…" and "Searching…".
    void setNetworkSearchRunning(bool running);

private slots:
    /// @brief Open directory or launch file when the user double-clicks / presses Enter.
    void onItemActivated(const QModelIndex &index);
    /// @brief Update the status bar with the current selection count.
    void onSelectionChanged();
    /// @brief Navigate to the path typed into the address bar.
    void onAddressBarReturn();
    /// @brief Show the right-click context menu at @p pos.
    void onContextMenuRequested(const QPoint &pos);

private:
    QLineEdit      *m_addressBar    = nullptr;
    QToolButton    *m_backButton    = nullptr;
    QToolButton    *m_forwardButton = nullptr;
    QTreeView      *m_fileView      = nullptr;
    QComboBox      *m_driveCombo    = nullptr;
    QLineEdit      *m_filterBar     = nullptr;
    QLabel         *m_emptyHint     = nullptr;  ///< "This folder is empty." overlay on the list viewport.
    QVector<QToolButton *> m_placeButtons;  ///< Desktop, Documents, … — path stored in "placePath" property.
    QList<DriveInfo> m_drives;                 ///< Last list from MainWindow — kept to rebuild the dropdown.
    bool             m_networkSearchRunning = false;
    QString          m_pendingNetworkPath;     ///< Target of a running openNetworkPath() check; cleared by navigateTo() / showFolder().

    QFileSystemModel       *m_model      = nullptr;
    WildcardFilterProxy    *m_proxyModel = nullptr;
    /// Not a QObject, so not parent-owned. Model is deleted first in ~FilePanel().
    std::unique_ptr<QAbstractFileIconProvider> m_iconProvider;

    QString          m_currentPath;
    QStack<QString>  m_backStack;
    QStack<QString>  m_forwardStack;
    QTimer          *m_filterDebounceTimer = nullptr;  ///< Debounces filter-bar input (150 ms).
    QTimer          *m_rowsChangedTimer    = nullptr;  ///< Batches empty hint / size bar updates while rows stream in.

    // Folder sizes: calculated on request on QtConcurrent, kept in the shared
    // FolderSizeCache. The info bar above the list offers / shows the calculation.
    FolderSizeCache *m_sizeCache          = nullptr;
    QToolButton    *m_placementButton     = nullptr;  ///< "Folders Mixed with Files ▾" — per-tab folder placement.
    QToolButton    *m_calcSizesButton     = nullptr;  ///< Σ — calculate all missing folder sizes.
    QFrame         *m_sizeBar             = nullptr;
    QLabel         *m_sizeBarLabel        = nullptr;
    QPushButton    *m_sizeBarCalcButton   = nullptr;
    QPushButton    *m_sizeBarCancelButton = nullptr;
    QToolButton    *m_sizeBarCloseButton  = nullptr;
    bool            m_sizeBarDismissed    = false;    ///< Closed with ✕ — hidden until the next folder change.
    QFutureWatcher<QVector<FolderSize>> *m_sizeWatcher       = nullptr;
    QTimer                              *m_sizeProgressTimer = nullptr;  ///< Refreshes the byte counter while calculating.
    QTimer                              *m_sizeRefreshTimer  = nullptr;  ///< Batches re-sort / totals while sizes stream in.
    std::shared_ptr<std::atomic<qint64>> m_sizeBytesSoFar;               ///< Written by the pool thread of the current run.
    int                                  m_sizeDone  = 0;
    int                                  m_sizeTotal = 0;

    void setupUi();
    /// @brief Show @p path without checking it and without a history entry —
    /// callers make sure it is a folder (start folder, back / forward, refresh fallback).
    void showFolder(const QString &path);
    /// @brief showFolder() plus a history entry — @p path is already known to be a folder.
    void enterFolder(const QString &path);
    /// @brief refresh() found m_currentPath gone — show @p fallback (its nearest existing parent), else home.
    void leaveMissingFolder(const QString &fallback);
    /// @brief Back / forward to a network folder: checked off the GUI thread, dropped from the history if gone.
    void showHistoryFolderChecked(const QString &path, bool back);
    /// @brief Point the view at m_currentPath again — the proxy mapping changes
    /// with every setRootPath() and every filter change.
    void syncRootIndex();
    /// @brief Build the (hidden) info bar for the folder size calculation.
    QFrame *createSizeBar();
    /// @brief Build short label for a drive entry (e.g. "C:").
    QString buildDriveLabel(const DriveInfo &drive) const;
    /// @brief Build multi-line tooltip with volume name, UNC target, FS type, and free space.
    QString buildDriveTooltip(const DriveInfo &drive) const;
    /// @brief Loop an input dialog until the user enters a valid name or cancels.
    QString promptValidName(const QString &title, const QString &label, const QString &suggestion, bool isDirectory,
                            const QString &parentDir, const QString &excludePath = {});
    void setupModel();
    void connectSignals();
    void setupKeyboardShortcuts();
    void updateAddressBar();
    /// @brief Emit "N items (size) | M selected (size) | path" — sizes include calculated folders.
    void updateStatusBar();
    void updateNavButtons();
    /// @brief Show an inline rename editor for the item at @p proxyIdx.
    void renameItem(const QModelIndex &proxyIdx);
    /// @brief Place selected paths on the system clipboard. @p isCut sets the MOVE flag.
    void copyToClipboard(bool isCut);
    /// @brief Rebuild the dropdown from m_drives plus the "Find network drives…" entry.
    void rebuildDriveCombo();
    /// @brief Select the dropdown entry of the drive that contains m_currentPath.
    void updateDriveSelection();
    /// @brief Navigate to the drive the user picked in the dropdown.
    void onDriveActivated(int index);
    /// @brief Dialog: enter "\\server", list its shares in the background, open the chosen one.
    void openServerDialog(const QString &initialServer = {});
    /// @brief Open a "\\server\share\…" path or a path on a mapped drive letter:
    /// reachability is checked off the GUI thread (first SMB access can take
    /// seconds); a server that wants a login gets Windows' sign-in dialog.
    void openNetworkPath(const QString &path);
    /// @brief openNetworkPath() found a server that wants credentials — sign in, then open.
    void signInToNetworkPath(const QString &target, const QString &shareRoot);
    /// @brief The network path is reachable — remember its share and navigate there.
    /// @p checked: the folder was already found on the pool, no second stat needed.
    void finishNetworkOpen(const QString &target, const QString &shareRoot, bool checked);
    /// @brief Give up on the pending network path; an empty @p message shows no error.
    void abortNetworkOpen(const QString &message);
    /// @brief navigateTo(), but a network path goes through openNetworkPath() first.
    void navigateChecked(const QString &path);
    /// @return The mapped network drive that @p path is on, or nullptr (no I/O).
    const DriveInfo *mappedDriveFor(const QString &path) const;
    /// @return True if @p path is a UNC path or on a mapped network drive (no I/O).
    bool isNetworkPath(const QString &path) const;
    /// @brief Add one icon button per user folder (Desktop, Documents, …) to @p layout.
    void setupPlaceButtons(QHBoxLayout *layout);
    /// @brief Create a quick-access button for @p location, or nullptr if missing/duplicate.
    QToolButton *createPlaceButton(QStandardPaths::StandardLocation location);
    /// @brief Check the place button whose path equals m_currentPath, uncheck all others.
    void updatePlaceSelection();
    /// @brief Restart the debounce timer when the filter text changes.
    void onFilterChanged(const QString &text);
    /// @brief Apply the current filter-bar text as a wildcard on the proxy model.
    void applyFilter();
    /// @brief Show "This folder is empty." / "No items match the filter." when applicable.
    void updateEmptyHint();
    /// @brief Space: (re)calculate the selected folders, or the current one if nothing is selected.
    void calculateSelectedFolderSizes();
    /// @brief Σ / Alt+Shift+Enter / "Calculate now": every visible folder without a size.
    void calculateAllFolderSizes();
    /// @return Visible folders of the current directory that have no size yet.
    QStringList uncalculatedFolders() const;
    /// @brief Calculate @p folders in the background; a running calculation is merged in.
    void startFolderSizeCalculation(const QStringList &folders);
    /// @brief Stop the running calculation (Esc, Cancel button, folder change, refresh).
    void cancelFolderSizeCalculation();
    bool isCalculatingFolderSizes() const;
    void onFolderSizeReady(int resultIndex);
    /// @brief The shared cache changed (this or another tab) — repaint now, re-sort / re-total batched.
    void scheduleSizeRefresh();
    void applySizeChanges();
    /// @return Name-column proxy indexes of all rows shown in the current folder.
    QModelIndexList visibleRows() const;

    /// Sum of item sizes; incomplete if a folder in it has no size yet.
    struct SizeTotal {
        qint64 bytes    = 0;
        bool   complete = true;
    };
    SizeTotal sizeTotal(const QModelIndexList &rows) const;
    /// @return "1.2 GB", "≥ 1.2 GB" (incomplete), or empty (nothing known yet).
    static QString formatSizeTotal(const SizeTotal &total);
    bool isActivePanel() const;
    void onFolderSizeCalculationFinished();
    /// @brief Show progress while calculating, or offer the calculation when sorted by size.
    void updateSizeBar();
    /// @brief Add the checkable "Folders on Top / at Bottom / Mixed" entries to @p menu.
    void addFolderPlacementActions(QMenu *menu);
    void setFolderPlacement(FolderPlacement placement);
    /// @brief Open a file with the OS default application.
    void openWithDefault(const QString &path);
    /// @brief Open a terminal window in @p dir (Windows Terminal, else PowerShell).
    void openTerminal(const QString &dir);
    /// @brief Handle middle-click on a directory to open it in a new tab.
    bool handleMiddleClickNavigation(QMouseEvent *mouseEvent);
};
