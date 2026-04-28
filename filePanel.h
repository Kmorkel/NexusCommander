#pragma once

#include <QWidget>
#include <QFileSystemModel>
#include <QListView>
#include <QLineEdit>
#include <QMimeData>
#include <QToolButton>
#include <QSortFilterProxyModel>
#include <QStack>
#include <QMouseEvent>
#include <QStorageInfo>
#include <QTimer>

/**
 * @brief One pane of the dual-panel file manager.
 *
 * Displays an address bar, back/forward navigation, a drive bar, a file list
 * (QListView backed by QFileSystemModel + WildcardFilterProxy), and a filter bar.
 * Two FilePanel instances are placed side-by-side in a QSplitter by MainWindow.
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
    explicit FilePanel(const QString &rootPath, QWidget *parent = nullptr);
    ~FilePanel() override = default;

    /// @return Absolute path of the directory currently shown.
    QString     currentPath() const;

    /// @return Absolute paths of all selected files/directories.
    QStringList selectedPaths() const;

    /// @brief Navigate to @p path, optionally recording a history entry.
    void navigateTo(const QString &path, bool addToHistory = true);

    /// @brief Strip trailing dots/spaces (Windows normalization).
    QString sanitizeFileName(const QString &name) const;

    /// @brief Validate a file/folder name for the current OS. Sets @p errorString on failure.
    bool isValidEntryName(const QString &name, QString &errorString) const;

    /// @brief Force re-read of the current directory.
    void refresh();

    /// @brief Prompt the user and create a new folder in the current directory.
    void createNewFolder();

    /// @brief Show or hide hidden/system files.
    void setShowHiddenFiles(bool visible);

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

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

public slots:
    void onGoBack();
    void onGoForward();

    /// @brief Called by MainWindow's centralized drive-watch timer.
    void onDriveTopologyChanged();

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
    QListView      *m_listView      = nullptr;
    QWidget        *m_driveBar      = nullptr;
    QLineEdit      *m_filterBar     = nullptr;

    QFileSystemModel       *m_model      = nullptr;
    WildcardFilterProxy    *m_proxyModel = nullptr;

    QString          m_currentPath;
    QStack<QString>  m_backStack;
    QStack<QString>  m_forwardStack;
    QTimer          *m_filterDebounceTimer = nullptr;  ///< Debounces filter-bar input (150 ms).

    void setupUi();
    /// @brief Build short label for a drive button (e.g. "C:").
    QString buildDriveLabel(const QStorageInfo &drive) const;
    /// @brief Build multi-line tooltip with volume name, FS type, and free space.
    QString buildDriveTooltip(const QStorageInfo &drive) const;
    /// @brief Create a drive-bar button wired to navigateTo().
    QToolButton* createDriveButton(const QStorageInfo &drive);
    /// @brief Loop an input dialog until the user enters a valid name or cancels.
    QString promptValidName(const QString &title, const QString &label, const QString &suggestion, bool isDirectory,
                            const QString &parentDir, const QString &excludePath = {});
    void setupModel();
    void connectSignals();
    void setupKeyboardShortcuts();
    void updateAddressBar();
    void updateStatusBar();
    void updateNavButtons();
    /// @brief Show an inline rename editor for the item at @p proxyIdx.
    void renameItem(const QModelIndex &proxyIdx);
    /// @brief Place selected paths on the system clipboard. @p isCut sets the MOVE flag.
    void copyToClipboard(bool isCut);
    /// @brief Delete all child widgets and layout items from @p layout.
    void clearLayout(QLayout *layout);
    /// @brief Rebuild the drive-bar buttons from QStorageInfo.
    void refreshDriveBar();
    /// @brief Restart the debounce timer when the filter text changes.
    void onFilterChanged(const QString &text);
    /// @brief Apply the current filter-bar text as a wildcard on the proxy model.
    void applyFilter();
    /// @brief Open a file with the OS default application.
    void openWithDefault(const QString &path);
    /// @brief Handle middle-click on a directory to open it in a new tab.
    bool handleMiddleClickNavigation(QMouseEvent *mouseEvent);
};
