#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QClipboard>
#include <QMimeData>
#include <QSortFilterProxyModel>
#include <QApplication>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QAbstractFileIconProvider>
#include <QFileIconProvider>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QImage>
#include <QStyle>
#include <QPushButton>
#include <QFuture>
#include <QHash>
#include <QHeaderView>
#include <QCollator>
#include <QLocale>
#include <QActionGroup>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui/private/qfilesystemmodel_p.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include "clipboardUtils.h"
#include "connectServerDialog.h"
#include "fileJob.h"
#include "filePanel.h"
#include "fileSystemUtils.h"
#include "networkDiscovery.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
constexpr int kNavButtonSize       = 28;
constexpr int kLayoutMargin        = 4;
constexpr int kLayoutSpacing       = 4;
// Debounce filter-bar typing — shorter feels laggy, longer feels unresponsive.
constexpr int kFilterDebounceMs    = 150;
constexpr int kEmptyHintTopMargin  = 12;
// QFileSystemModel columns.
constexpr int kNameColumn          = 0;
constexpr int kSizeColumn          = 1;
constexpr int kTypeColumn          = 2;
constexpr int kDateColumn          = 3;
constexpr int kSizeColumnWidth     = 90;
constexpr int kTypeColumnWidth     = 120;
constexpr int kDateColumnWidth     = 130;
// Folder sizes stream in one by one — re-sort at most this often, refresh the byte counter this often.
constexpr int kSizeRefreshDelayMs     = 300;
constexpr int kSizeProgressIntervalMs = 250;
// Rows inserted / removed (file watcher, a copy into this folder) are handled at most this often.
constexpr int kRowsChangedDelayMs     = 100;
// Marks the action entries at the end of the drive dropdown — Qt::UserRole holds the drive root.
constexpr int kDriveActionRole     = Qt::UserRole + 1;
enum class DriveAction { None, FindNetwork, ConnectServer };
// Folder placement of a new tab — changeable per tab, saved with the tab.
constexpr FolderPlacement kDefaultFolderPlacement = FolderPlacement::Mixed;

// UNC path or a mapped drive letter. GetDriveTypeW() asks the local redirector,
// not the server — safe on the model's gatherer thread.
bool isOnNetworkDrive(const QString &absolutePath)
{
    const QString path = QDir::fromNativeSeparators(absolutePath);
    if (path.startsWith(QLatin1String("//")))
        return true;
#ifdef Q_OS_WIN
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        const wchar_t root[] = {wchar_t(path.at(0).unicode()), L':', L'\\', L'\0'};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
#endif
    return false;
}

// Extensions whose shell icon is read from the file itself — Qt caches none of them.
bool hasOwnShellIcon(const QFileInfo &info)
{
    const QString suffix = info.suffix();
    return suffix.compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0
        || suffix.compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0
        || suffix.compare(QLatin1String("ico"), Qt::CaseInsensitive) == 0;
}

// "1,234,567,890 bytes" for tooltips — the exact number behind "1.2 GB".
QString exactBytes(qint64 bytes)
{
    return QObject::tr("%1 bytes").arg(QLocale().toString(bytes));
}

// INI value of a folder placement — not translated, unlike folderPlacementText().
// INI value of a list column — readable instead of the column number.
constexpr std::pair<int, const char *> kColumnKeys[] = {
    {kNameColumn, "name"}, {kSizeColumn, "size"}, {kTypeColumn, "type"}, {kDateColumn, "date"},
};

QString columnKey(int column)
{
    for (const auto &[index, key] : kColumnKeys) {
        if (index == column)
            return QLatin1String(key);
    }
    return QLatin1String("name");
}

// @return The column named @p key, or -1 if unknown.
int columnFromKey(const QString &key)
{
    for (const auto &[index, columnName] : kColumnKeys) {
        if (key == QLatin1String(columnName))
            return index;
    }
    return -1;
}

QString folderPlacementKey(FolderPlacement placement)
{
    switch (placement) {
    case FolderPlacement::Top:    return QStringLiteral("top");
    case FolderPlacement::Bottom: return QStringLiteral("bottom");
    case FolderPlacement::Mixed:  return QStringLiteral("mixed");
    }
    return {};
}

FolderPlacement folderPlacementFromKey(const QString &key)
{
    if (key == QLatin1String("top"))    return FolderPlacement::Top;
    if (key == QLatin1String("bottom")) return FolderPlacement::Bottom;
    if (key == QLatin1String("mixed"))  return FolderPlacement::Mixed;
    return kDefaultFolderPlacement;
}

QString folderPlacementText(FolderPlacement placement)
{
    switch (placement) {
    case FolderPlacement::Top:    return QObject::tr("Folders on Top");
    case FolderPlacement::Bottom: return QObject::tr("Folders at Bottom");
    case FolderPlacement::Mixed:  return QObject::tr("Folders Mixed with Files");
    }
    return {};
}

// How far a network path could be reached — decides between opening it, an
// error message, and Windows' sign-in dialog.
enum class Reach { Ok, FolderMissing, DriveUnreachable, ShareMissing, ServerFailed, LoginRequired };

struct ReachCheck {
    Reach                         reach       = Reach::Ok;
    NetworkDiscovery::ServerState serverState = NetworkDiscovery::ServerState::Reachable;  ///< Why ServerFailed.
};

// "//nas/media/…" → "nas".
QString serverOf(const QString &uncPath)
{
    return uncShareRoot(uncPath).section('/', 2, 2);
}

// QFileSystemModel stats the server root ("\\nas") on the GUI thread the first
// time a model shows a folder on that host. Qt answers that via NetShareEnum —
// an RPC call to the server that took seconds every time ("Not Responding";
// setting up the connection on the pool first did not help). So the root is
// stat'ed on the pool once per session and handed to each model by
// FilePanel::addServerNode(). GUI thread only. Key: pathKey() of the server name.
QHash<QString, QFileInfo> &serverRoots()
{
    static QHash<QString, QFileInfo> roots;
    return roots;
}

// Blocking — NetShareEnum. The server root of @p target with all its data
// cached, so copies answer on the GUI thread without I/O.
QFileInfo statServerRoot(const QString &target)
{
    QFileInfo info(QStringLiteral("//") + serverOf(target));
    info.stat();
    return info;
}

// Blocking — the first access to a share sets up the SMB session and can take
// ~10 s (longer if the disk sleeps). That includes a mapped letter: Windows
// reconnects it on the first access.
// @p root: what has to answer for @p target to be reachable ("//nas/media" or "Z:/").
// @p shareRoot / @p server: the share behind it — empty for a letter whose share is unknown.
ReachCheck checkNetworkPath(const QString &target, const QString &root,
                            const QString &shareRoot, const QString &server)
{
    if (QFileInfo(target).isDir())
        return {Reach::Ok};
    // Share reachable but sub-folder missing = typo — no sign-in prompt for that.
    if (QFileInfo(root).isDir())
        return {Reach::FolderMissing};
    // Mapped letter whose share is unknown — nothing more to check.
    if (server.isEmpty())
        return {Reach::DriveUnreachable};

    const NetworkDiscovery::ServerState state = NetworkDiscovery::probeServer(server);
    if (state != NetworkDiscovery::ServerState::Reachable)
        return {Reach::ServerFailed, state};

    // Server answers. If it lists its shares without a login, the share
    // name is simply wrong; only a refused listing means credentials.
    const NetworkDiscovery::ShareListing listing = NetworkDiscovery::listShares(server);
    if (listing.loginRequired)
        return {Reach::LoginRequired};
    const bool listed = std::any_of(listing.uncPaths.cbegin(), listing.uncPaths.cend(),
                                    [&shareRoot](const QString &path) {
                                        return path.compare(shareRoot, Qt::CaseInsensitive) == 0;
                                    });
    // Listed but not accessible = per-share permissions — that needs a login too.
    return {listed ? Reach::LoginRequired : Reach::ShareMissing};
}

// @p path itself if it is still a folder, else its nearest existing parent; empty if none.
// Blocking — run it on the pool for network paths.
QString existingFolderOrParent(const QString &path)
{
    // Walked as strings — QDir::cdUp() stops at the first parent that is missing too.
    QString candidate = QDir::cleanPath(QDir(path).absolutePath());
    for (;;) {
        if (QFileInfo(candidate).isDir())
            return candidate;
        // Not above the share — "\\server" alone is no folder.
        if (isSharePath(candidate) && pathEquals(candidate, uncShareRoot(candidate)))
            return {};
        const QString parent = QFileInfo(candidate).absolutePath();
        if (parent == candidate)
            return {};   // drive root reached
        candidate = parent;
    }
}
}

// Filter + sort proxy. Passes through ancestors of the current root (so
// mapFromSource stays valid) and applies the wildcard filter to everything else.
// Optionally hides ".part" files (View → Hide .part Files).
// Sorting keeps folders on top / at the bottom / mixed (per tab) and sorts
// folders by their calculated size (from the shared FolderSizeCache), which
// QFileSystemModel does not know.
class WildcardFilterProxy : public QSortFilterProxyModel
{
public:
    explicit WildcardFilterProxy(const FolderSizeCache *sizeCache, QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
        , m_sizeCache{sizeCache}
    {
        // Natural order like Explorer: "file2" before "file10".
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    }

    void setCurrentPath(const QString &path) { m_currentPath = path; }

    FolderPlacement folderPlacement() const { return m_folderPlacement; }
    void setFolderPlacement(FolderPlacement placement) { m_folderPlacement = placement; }

    // Folders this panel is calculating — shown as "…". Keys are QFileSystemModel::filePath() values.
    bool isDirPending(const QString &path) const  { return m_pendingDirs.contains(path); }
    QStringList pendingDirs() const { return QStringList(m_pendingDirs.cbegin(), m_pendingDirs.cend()); }
    void setPendingDirs(const QStringList &paths) { m_pendingDirs = QSet<QString>(paths.cbegin(), paths.cend()); }
    void removePendingDir(const QString &path)    { m_pendingDirs.remove(path); }
    void clearPendingDirs()                       { m_pendingDirs.clear(); }

    bool hidesPartFiles() const { return m_hidePartFiles; }
    void setHidePartFiles(bool hide)
    {
        // invalidateFilter() is deprecated since Qt 6.10 — only rows change here.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_hidePartFiles = hide;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        m_hidePartFiles = hide;
        invalidateFilter();
#endif
    }

    // Size of the item at @p proxyIndex, -1 for a folder without a calculated size.
    qint64 sizeBytes(const QModelIndex &proxyIndex) const
    {
        const QModelIndex sourceIndex = mapToSource(proxyIndex);
        return sizeOf(sourceIndex, fileSystemModel()->isDir(sourceIndex));
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (index.column() != kSizeColumn || (role != Qt::DisplayRole && role != Qt::ToolTipRole))
            return QSortFilterProxyModel::data(index, role);

        const auto *fs = fileSystemModel();
        const QModelIndex sourceIndex = mapToSource(index);
        if (!fs->isDir(sourceIndex)) {
            const qint64 bytes = fs->size(sourceIndex);
            return role == Qt::DisplayRole ? formatSize(bytes) : exactBytes(bytes);
        }

        const QString path = fs->filePath(sourceIndex);
        const std::optional<FolderStats> stats = m_sizeCache->stats(path);
        const bool pending = m_pendingDirs.contains(path);

        if (role == Qt::ToolTipRole) {
            if (stats) {
                return QObject::tr("%1\n%2 files in %3 folders")
                    .arg(exactBytes(stats->bytes),
                         QLocale().toString(stats->files),
                         QLocale().toString(stats->folders));
            }
            return pending ? QVariant() : QVariant(QObject::tr("Press Space to calculate the folder size"));
        }
        if (stats)
            return formatSize(stats->bytes);
        return pending ? QStringLiteral("…") : QStringLiteral("<DIR>");
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const auto *fs = fileSystemModel();
        const bool leftIsDir  = fs->isDir(left);
        const bool rightIsDir = fs->isDir(right);

        if (leftIsDir != rightIsDir && m_folderPlacement != FolderPlacement::Mixed) {
            const bool leftFirst = leftIsDir == (m_folderPlacement == FolderPlacement::Top);
            // Descending order calls lessThan() with swapped arguments — compensate
            // so the folder placement does not flip with the sort direction.
            return sortOrder() == Qt::AscendingOrder ? leftFirst : !leftFirst;
        }

        int order = 0;
        switch (left.column()) {
        case kSizeColumn: {
            const qint64 leftSize  = sizeOf(left, leftIsDir);
            const qint64 rightSize = sizeOf(right, rightIsDir);
            order = leftSize < rightSize ? -1 : (leftSize > rightSize ? 1 : 0);
            break;
        }
        case kTypeColumn:
            order = m_collator.compare(fs->type(left), fs->type(right));
            break;
        case kDateColumn: {
            const QDateTime leftDate  = fs->lastModified(left);
            const QDateTime rightDate = fs->lastModified(right);
            order = leftDate < rightDate ? -1 : (leftDate > rightDate ? 1 : 0);
            break;
        }
        default:
            break;
        }

        // Name column, and ties in the other columns. fileName() reads the
        // display text, so it must be asked on the name column.
        if (order == 0)
            order = m_collator.compare(fs->fileName(left.siblingAtColumn(kNameColumn)),
                                       fs->fileName(right.siblingAtColumn(kNameColumn)));
        return order < 0;
    }

    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto *fs = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (fs && !m_currentPath.isEmpty()) {
            const QString itemPath = fs->filePath(fs->index(row, kNameColumn, parent));
            // Pass through ancestors of current root so mapFromSource stays valid
            if (isSameOrInside(m_currentPath, itemPath))
                return true;
        }
        // Unfinished downloads — copy / move leave them out too while hidden.
        if (fs && m_hidePartFiles) {
            const QModelIndex index = fs->index(row, kNameColumn, parent);
            if (!fs->isDir(index) && isPartFileName(fs->fileName(index)))
                return false;
        }
        return QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }

private:
    const FolderSizeCache  *m_sizeCache;
    QString                 m_currentPath;
    FolderPlacement         m_folderPlacement = kDefaultFolderPlacement;
    QCollator               m_collator;
    QSet<QString>           m_pendingDirs;
    bool                    m_hidePartFiles   = false;

    const QFileSystemModel *fileSystemModel() const
    {
        return static_cast<const QFileSystemModel *>(sourceModel());
    }

    // Takes a source index. Folders without a calculated size sort as smallest.
    qint64 sizeOf(const QModelIndex &index, bool isDir) const
    {
        const auto *fs = fileSystemModel();
        if (!isDir)
            return fs->size(index);
        const std::optional<FolderStats> stats = m_sizeCache->stats(fs->filePath(index));
        return stats ? stats->bytes : -1;
    }
};

// Wraps the shell icon of a directory. Windows fails to deliver icons for some
// special folders (e.g. C:\Users, the profile dir) and Qt then paints nothing.
// The shell icon loads lazily and QIcon::isNull() is always false for it, so
// the failure can only be detected here, at pixmap time.
class FolderFallbackIconEngine : public QIconEngine
{
public:
    FolderFallbackIconEngine(const QIcon &shellIcon, const QIcon &fallbackIcon)
        : m_shellIcon{shellIcon}
        , m_fallbackIcon{fallbackIcon}
    {}

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        if (!m_shellFailed) {
            const QPixmap shellPixmap = m_shellIcon.pixmap(size, scale, mode, state);
            if (!shellPixmap.isNull())
                return shellPixmap;
            // Remember the failure — Qt does not cache null results and would
            // ask the shell again on every repaint.
            m_shellFailed = true;
        }
        return m_fallbackIcon.pixmap(size, scale, mode, state);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state,
                                               painter->device()->devicePixelRatio()));
    }

    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override
    {
        return m_shellIcon.availableSizes(mode, state);
    }

    QIconEngine *clone() const override
    {
        return new FolderFallbackIconEngine(m_shellIcon, m_fallbackIcon);
    }

private:
    QIcon m_shellIcon;
    QIcon m_fallbackIcon;
    bool  m_shellFailed = false;
};

#ifdef Q_OS_WIN
// Shell icon of a file type, looked up by the extension alone
// (SHGFI_USEFILEATTRIBUTES) — the file itself is never touched. Cached per
// extension and size, like Qt does for its own file icons.
class SuffixIconEngine : public QIconEngine
{
public:
    SuffixIconEngine(const QString &suffix, const QIcon &fallbackIcon)
        : m_suffix{suffix.toLower()}
        , m_fallbackIcon{fallbackIcon}
    {}

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        const bool large  = size.width() * scale > GetSystemMetrics(SM_CXSMICON);
        const QString key = QStringLiteral("nc_suffix_%1_%2").arg(m_suffix, large ? "l" : "s");

        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap))
            return pixmap;

        const QString dummyName = QStringLiteral("file.") + m_suffix;
        const UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
        SHFILEINFOW info = {};
        if (SHGetFileInfoW(reinterpret_cast<const wchar_t *>(dummyName.utf16()), FILE_ATTRIBUTE_NORMAL,
                           &info, sizeof(info), flags) && info.hIcon) {
            pixmap = QPixmap::fromImage(QImage::fromHICON(info.hIcon));
            DestroyIcon(info.hIcon);
        }
        if (pixmap.isNull())
            return m_fallbackIcon.pixmap(size, scale, mode, state);
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state,
                                               painter->device()->devicePixelRatio()));
    }

    QIconEngine *clone() const override
    {
        return new SuffixIconEngine(m_suffix, m_fallbackIcon);
    }

private:
    QString m_suffix;
    QIcon   m_fallbackIcon;
};
#endif

// Icon provider for the model: directories fall back to the default folder icon
// when the Windows API delivers none.
// Shell icons are loaded at paint time on the GUI thread. Qt caches none for
// folders and .exe / .lnk / .ico — every repaint asks the shell per row. On a
// network drive that is an SMB round trip each, and while a copy saturates the
// link the GUI froze (even when just moving the window). There these entries
// get the generic icons instead; local entries keep their shell icons.
// Other files on a network drive get their type's icon by extension: Qt asks
// the shell with the full path for the first file of each type and waits up
// to seconds for the answer on the GUI thread.
class FolderFallbackIconProvider : public QFileIconProvider
{
public:
    // Generic icons are resolved once here on the GUI thread; icon() runs on
    // the model's gatherer thread.
    FolderFallbackIconProvider()
        : m_folderIcon{QFileIconProvider::icon(QAbstractFileIconProvider::Folder)}
        , m_fileIcon{QFileIconProvider::icon(QAbstractFileIconProvider::File)}
    {}

    using QFileIconProvider::icon;

    QIcon icon(const QFileInfo &info) const override
    {
        if (isOnNetworkDrive(info.absoluteFilePath())) {
            if (info.isDir() || hasOwnShellIcon(info))
                return info.isDir() ? m_folderIcon : m_fileIcon;
#ifdef Q_OS_WIN
            return QIcon(new SuffixIconEngine(info.suffix(), m_fileIcon));
#endif
        }

        const QIcon shellIcon = QFileIconProvider::icon(info);
        if (!info.isDir())
            return shellIcon;
        return QIcon(new FolderFallbackIconEngine(shellIcon, m_folderIcon));
    }

private:
    QIcon m_folderIcon;
    QIcon m_fileIcon;
};

// Drive dropdown that reports when its list is about to open. Network drives
// are probed only then — never in the background. Plain callback instead of a
// signal so the class needs no Q_OBJECT / moc in this .cpp.
class DriveComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

    std::function<void()> onPopupAboutToShow;

    void showPopup() override
    {
        if (onPopupAboutToShow)
            onPopupAboutToShow();
        QComboBox::showPopup();
    }
};

FilePanel::FilePanel(const QString &rootPath, FolderSizeCache *sizeCache, QWidget *parent)
    : QWidget(parent)
    , m_sizeCache(sizeCache)
{
    setupModel();
    setupUi();
    connectSignals();
    setupKeyboardShortcuts();
    showFolder(rootPath);
}

FilePanel::~FilePanel()
{
    // Let a running size calculation stop at its next entry — it never touches the panel.
    m_sizeWatcher->cancel();
    // The model's gatherer thread calls m_iconProvider — delete the model first
    // so the provider is never used after the member has been destroyed.
    delete m_model;
}

void FilePanel::setupModel()
{
    m_model = new QFileSystemModel(this);
    // Set before setRootPath() so the very first icons already use the fallback.
    m_iconProvider = std::make_unique<FolderFallbackIconProvider>();
    m_model->setIconProvider(m_iconProvider.get());
    m_model->setRootPath("");
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    m_proxyModel = new WildcardFilterProxy(m_sizeCache, this);
    m_proxyModel->setSourceModel(m_model);
    m_proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxyModel->setFilterKeyColumn(0);
    m_proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void FilePanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    // No side margins: address bar, list and filter bar reach the panel's edges.
    mainLayout->setContentsMargins(0, kLayoutMargin, 0, kLayoutMargin);
    mainLayout->setSpacing(kLayoutSpacing);

    auto *addressBarLayout = new QHBoxLayout;

    m_backButton = new QToolButton(this);
    m_backButton->setIcon(QIcon::fromTheme("go-previous"));
    m_backButton->setToolTip(tr("Back (Alt+Left)"));
    m_backButton->setFixedSize(kNavButtonSize, kNavButtonSize);
    m_backButton->setEnabled(false);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setIcon(QIcon::fromTheme("go-next"));
    m_forwardButton->setToolTip(tr("Forward (Alt+Right)"));
    m_forwardButton->setFixedSize(kNavButtonSize, kNavButtonSize);
    m_forwardButton->setEnabled(false);

    m_addressBar = new QLineEdit(this);
    m_addressBar->setPlaceholderText(tr("Path…"));

    auto *driveCombo = new DriveComboBox(this);
    driveCombo->onPopupAboutToShow = [this] { emit driveScanRequested(); };
    m_driveCombo = driveCombo;
    m_driveCombo->setToolTip(tr("Drive"));
    // Width follows the longest entry and is recomputed on every refresh.
    m_driveCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    // Keyboard focus stays in the list view — Tab switches panels, not widgets.
    m_driveCombo->setFocusPolicy(Qt::ClickFocus);

    addressBarLayout->addWidget(m_backButton);
    addressBarLayout->addWidget(m_forwardButton);
    addressBarLayout->addWidget(m_addressBar);
    mainLayout->addLayout(addressBarLayout);

    auto *driveRowLayout = new QHBoxLayout;
    driveRowLayout->addWidget(m_driveCombo);
    driveRowLayout->addSpacing(kLayoutSpacing * 2);
    setupPlaceButtons(driveRowLayout);
    // Stretch keeps combo and buttons at their size hint instead of spreading out.
    driveRowLayout->addStretch();

    // Folder placement of this tab — the menu is rebuilt on open so the check
    // mark matches a change made via the list's context menu.
    m_placementButton = new QToolButton(this);
    m_placementButton->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
    m_placementButton->setText(folderPlacementText(kDefaultFolderPlacement));
    m_placementButton->setToolTip(tr("Where folders appear in the sorted list (this tab only)"));
    m_placementButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_placementButton->setPopupMode(QToolButton::InstantPopup);
    m_placementButton->setAutoRaise(true);
    m_placementButton->setFocusPolicy(Qt::NoFocus);
    auto *placementMenu = new QMenu(m_placementButton);
    connect(placementMenu, &QMenu::aboutToShow, this, [this, placementMenu] {
        qDeleteAll(placementMenu->findChildren<QActionGroup *>());
        placementMenu->clear();
        addFolderPlacementActions(placementMenu);
    });
    m_placementButton->setMenu(placementMenu);

    m_calcSizesButton = new QToolButton(this);
    m_calcSizesButton->setText(QStringLiteral("Σ"));
    m_calcSizesButton->setToolTip(tr("Calculate all folder sizes (Alt+Shift+Enter)"));
    m_calcSizesButton->setFixedSize(kNavButtonSize, kNavButtonSize);
    m_calcSizesButton->setAutoRaise(true);
    m_calcSizesButton->setFocusPolicy(Qt::NoFocus);
    QFont sigmaFont = m_calcSizesButton->font();
    sigmaFont.setBold(true);
    m_calcSizesButton->setFont(sigmaFont);
    connect(m_calcSizesButton, &QToolButton::clicked, this, [this] {
        calculateAllFolderSizes();
        m_fileView->setFocus();
    });

    driveRowLayout->addWidget(m_placementButton);
    driveRowLayout->addWidget(m_calcSizesButton);
    // Entries arrive via setDrives() once MainWindow's background scan finishes.
    mainLayout->addLayout(driveRowLayout);

    mainLayout->addWidget(createSizeBar());

    m_fileView = new QTreeView(this);
    m_fileView->setModel(m_proxyModel);
    m_fileView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Flat list with columns — never expanded, folders open on activation.
    m_fileView->setRootIsDecorated(false);
    m_fileView->setItemsExpandable(false);
    m_fileView->setExpandsOnDoubleClick(false);
    m_fileView->setUniformRowHeights(true);
    m_fileView->setAllColumnsShowFocus(true);
    m_fileView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fileView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_fileView->setDragDropMode(QAbstractItemView::NoDragDrop);
    // Stylesheet is parsed ONCE here; setActive() only flips the dynamic
    // property below, which avoids re-parsing on every focus change.
    m_fileView->setStyleSheet(
        R"(QTreeView[panelActive="true"] { background-color: rgba(255, 255, 255, 0.08); })");
    m_fileView->setProperty("panelActive", false);

    QHeaderView *header = m_fileView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
    header->resizeSection(kSizeColumn, kSizeColumnWidth);
    header->resizeSection(kTypeColumn, kTypeColumnWidth);
    header->resizeSection(kDateColumn, kDateColumnWidth);

    // Click on a column header sorts; the proxy does the actual comparison.
    m_fileView->setSortingEnabled(true);
    m_fileView->sortByColumn(kNameColumn, Qt::AscendingOrder);
    mainLayout->addWidget(m_fileView, 1);

    // Overlay on the viewport like Explorer's "This folder is empty." —
    // geometry follows the viewport in eventFilter (QEvent::Resize).
    m_emptyHint = new QLabel(m_fileView->viewport());
    m_emptyHint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_emptyHint->setForegroundRole(QPalette::PlaceholderText);
    // Clicks pass through so the context menu (Paste, New Folder) still works.
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyHint->hide();

    m_filterBar = new QLineEdit(this);
    m_filterBar->setPlaceholderText(tr("Filter… (*.txt, image*, foo?)"));
    m_filterBar->setClearButtonEnabled(true);
    mainLayout->addWidget(m_filterBar);

    setFocusProxy(m_fileView);
}

QFrame *FilePanel::createSizeBar()
{
    m_sizeBar = new QFrame(this);
    m_sizeBar->setFrameShape(QFrame::StyledPanel);
    // Tooltip colours: a hint strip like a browser's info bar, in light and dark themes.
    m_sizeBar->setAutoFillBackground(true);
    m_sizeBar->setBackgroundRole(QPalette::ToolTipBase);

    m_sizeBarLabel = new QLabel(m_sizeBar);
    m_sizeBarLabel->setForegroundRole(QPalette::ToolTipText);

    m_sizeBarCalcButton   = new QPushButton(tr("Calculate now"), m_sizeBar);
    m_sizeBarCancelButton = new QPushButton(tr("Cancel"), m_sizeBar);
    m_sizeBarCloseButton  = new QToolButton(m_sizeBar);
    m_sizeBarCloseButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    m_sizeBarCloseButton->setAutoRaise(true);
    m_sizeBarCloseButton->setToolTip(tr("Hide until the folder is changed"));

    // Keyboard focus stays in the list view — Tab switches panels, not widgets.
    for (QWidget *button : {static_cast<QWidget *>(m_sizeBarCalcButton),
                            static_cast<QWidget *>(m_sizeBarCancelButton),
                            static_cast<QWidget *>(m_sizeBarCloseButton)})
        button->setFocusPolicy(Qt::NoFocus);

    auto *layout = new QHBoxLayout(m_sizeBar);
    layout->setContentsMargins(kLayoutMargin * 2, kLayoutMargin / 2, kLayoutMargin, kLayoutMargin / 2);
    layout->addWidget(m_sizeBarLabel, 1);
    layout->addWidget(m_sizeBarCalcButton);
    layout->addWidget(m_sizeBarCancelButton);
    layout->addWidget(m_sizeBarCloseButton);

    connect(m_sizeBarCalcButton, &QPushButton::clicked, this, [this] {
        calculateAllFolderSizes();
        m_fileView->setFocus();
    });
    connect(m_sizeBarCancelButton, &QPushButton::clicked,
            this,                  &FilePanel::cancelFolderSizeCalculation);
    connect(m_sizeBarCloseButton, &QToolButton::clicked, this, [this] {
        m_sizeBarDismissed = true;
        updateSizeBar();
    });

    // Shown by updateSizeBar() only when sorted by size or while calculating.
    m_sizeBar->hide();
    return m_sizeBar;
}

void FilePanel::connectSignals()
{
    // Created first — rowsInserted / directoryLoaded below already call updateSizeBar().
    m_sizeWatcher = new QFutureWatcher<QVector<FolderSize>>(this);
    connect(m_sizeWatcher, &QFutureWatcherBase::resultReadyAt, this, &FilePanel::onFolderSizeReady);
    connect(m_sizeWatcher, &QFutureWatcherBase::finished,
            this,          &FilePanel::onFolderSizeCalculationFinished);

    m_sizeProgressTimer = new QTimer(this);
    m_sizeProgressTimer->setInterval(kSizeProgressIntervalMs);
    connect(m_sizeProgressTimer, &QTimer::timeout, this, &FilePanel::updateSizeBar);

    m_sizeRefreshTimer = new QTimer(this);
    m_sizeRefreshTimer->setSingleShot(true);
    m_sizeRefreshTimer->setInterval(kSizeRefreshDelayMs);
    connect(m_sizeRefreshTimer, &QTimer::timeout, this, &FilePanel::applySizeChanges);

    // Sizes calculated in any tab (or invalidated by F2 / a finished job) show up here too.
    connect(m_sizeCache, &FolderSizeCache::changed, this, &FilePanel::scheduleSizeRefresh);

    connect(m_fileView->header(), &QHeaderView::sortIndicatorChanged, this, &FilePanel::updateSizeBar);

    connect(m_fileView, &QTreeView::activated,
            this,   &FilePanel::onItemActivated);
    connect(m_fileView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FilePanel::onSelectionChanged);
    connect(m_fileView, &QTreeView::customContextMenuRequested,
            this,   &FilePanel::onContextMenuRequested);
    connect(m_addressBar, &QLineEdit::returnPressed,
            this,         &FilePanel::onAddressBarReturn);
    // activated (not currentIndexChanged) — fires only on user choice, never
    // when updateDriveSelection() syncs the combo programmatically.
    connect(m_driveCombo, &QComboBox::activated,
            this,         &FilePanel::onDriveActivated);
    connect(m_backButton,    &QToolButton::clicked, this, &FilePanel::onGoBack);
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePanel::onGoForward);

    connect(m_model, &QFileSystemModel::directoryLoaded,
            this, [this](const QString &loadedPath) {
                if (QDir::cleanPath(loadedPath) == m_currentPath) {
                    updateStatusBar();
                    updateSizeBar();
                }
            });

    // External changes (file watcher) can empty or fill the folder. A copy
    // into the shown folder inserts rows one by one — handle them in batches,
    // updateSizeBar() walks every row and would otherwise run per file.
    m_rowsChangedTimer = new QTimer(this);
    m_rowsChangedTimer->setSingleShot(true);
    m_rowsChangedTimer->setInterval(kRowsChangedDelayMs);
    connect(m_rowsChangedTimer, &QTimer::timeout, this, [this] {
        updateEmptyHint();
        updateSizeBar();
    });
    // Not restarted while running — a steady stream of rows still updates every interval.
    const auto scheduleRowsChanged = [this] {
        if (!m_rowsChangedTimer->isActive())
            m_rowsChangedTimer->start();
    };
    connect(m_proxyModel, &QSortFilterProxyModel::rowsInserted, this, scheduleRowsChanged);
    connect(m_proxyModel, &QSortFilterProxyModel::rowsRemoved,  this, scheduleRowsChanged);

    // viewport() must be filtered separately because mouse events (e.g. middle-click)
    // are delivered to the viewport, not to the QTreeView itself.
    m_fileView->installEventFilter(this);
    m_fileView->viewport()->installEventFilter(this);
    m_filterBar->installEventFilter(this);

    m_filterDebounceTimer = new QTimer(this);
    m_filterDebounceTimer->setSingleShot(true);
    m_filterDebounceTimer->setInterval(kFilterDebounceMs);
    connect(m_filterDebounceTimer, &QTimer::timeout, this, &FilePanel::applyFilter);

    connect(m_filterBar, &QLineEdit::textChanged, this, &FilePanel::onFilterChanged);
}

void FilePanel::setupKeyboardShortcuts()
{
    const auto bindShortcut = [this](QKeySequence key, auto slot) {
        auto *shortcut = new QShortcut(key, this, slot);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    };

    bindShortcut(QKeySequence::Copy,      [this] { copyToClipboard(false); });
    bindShortcut(QKeySequence::Cut,       [this] { copyToClipboard(true); });
    bindShortcut(QKeySequence::Paste,     [this] { emit pasteRequested(); });
    bindShortcut(QKeySequence::Delete,    [this] { emit deleteRequested(); });
    bindShortcut(QKeySequence::SelectAll, [this] { m_fileView->selectAll(); });
    bindShortcut(QKeySequence::Back,      [this] { onGoBack(); });
    bindShortcut(QKeySequence::Forward,   [this] { onGoForward(); });

    bindShortcut(Qt::Key_Backspace, [this] {
        // Parent from the string alone — QDir::cdUp() stats it on the GUI thread.
        const QString parent = QFileInfo(m_currentPath).absolutePath();
        if (!pathEquals(parent, m_currentPath) && !isServerOnlyPath(parent))
            navigateChecked(parent);
    });

    bindShortcut(Qt::CTRL | Qt::Key_S, [this] {
        m_filterBar->setFocus();
        m_filterBar->selectAll();
    });

    auto *escShortcut = new QShortcut(Qt::Key_Escape, m_filterBar);
    escShortcut->setContext(Qt::WidgetShortcut);

    connect(escShortcut, &QShortcut::activated, this, [this] {
        m_filterBar->clear();
        m_fileView->setFocus();
    });

    // Folder sizes — bound to the list only, so Space still types a space in
    // the address and filter bars.
    const auto bindListShortcut = [this](QKeySequence key, auto slot) {
        auto *shortcut = new QShortcut(key, m_fileView, slot);
        shortcut->setContext(Qt::WidgetShortcut);
    };

    bindListShortcut(Qt::Key_Space,  [this] { calculateSelectedFolderSizes(); });
    bindListShortcut(Qt::Key_Escape, [this] { cancelFolderSizeCalculation(); });
    bindListShortcut(Qt::ALT | Qt::SHIFT | Qt::Key_Return, [this] { calculateAllFolderSizes(); });
}

QString FilePanel::currentPath() const { return m_currentPath; }

QStringList FilePanel::selectedPaths() const
{
    // selectedRows() — with SelectRows, selectedIndexes() would return one index per column.
    QStringList paths;
    for (const QModelIndex &proxyIdx : m_fileView->selectionModel()->selectedRows(kNameColumn)) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        paths << m_model->filePath(srcIdx);
    }
    return paths;
}

void FilePanel::advanceCursorPastSelection()
{
    const QModelIndexList rows = m_fileView->selectionModel()->selectedRows(kNameColumn);
    if (rows.isEmpty())
        return;

    // Selected rows are not ordered — find the lowest one in view order.
    int lastRow = -1;
    for (const QModelIndex &idx : rows)
        lastRow = qMax(lastRow, idx.row());

    const QModelIndex root = m_fileView->rootIndex();
    const int targetRow    = qMin(lastRow + 1, m_proxyModel->rowCount(root) - 1);
    const QModelIndex next = m_proxyModel->index(targetRow, kNameColumn, root);
    if (!next.isValid())
        return;

    m_fileView->selectionModel()->setCurrentIndex(
        next, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_fileView->scrollTo(next);
}

void FilePanel::navigateTo(const QString &path)
{
    // Any explicit navigation supersedes a network path that is still being checked.
    m_pendingNetworkPath.clear();

    const QFileInfo info(path);
    if (!info.exists()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Path does not exist: %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!info.isDir()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Not a directory: %1").arg(QDir::toNativeSeparators(path)));
        return;
    }

    enterFolder(path);
}

void FilePanel::enterFolder(const QString &path)
{
    const QString cleanPath = QDir::cleanPath(path);
    if (!m_currentPath.isEmpty() && m_currentPath != cleanPath) {
        m_backStack.push(m_currentPath);
        m_forwardStack.clear();
    }
    showFolder(cleanPath);
}

void FilePanel::showFolder(const QString &path)
{
    m_pendingNetworkPath.clear();

    // Sizes already calculated stay cached; a running calculation belongs to the old folder.
    cancelFolderSizeCalculation();
    m_sizeBarDismissed = false;

    m_currentPath = QDir::cleanPath(path);
    m_proxyModel->setCurrentPath(m_currentPath);

    m_filterDebounceTimer->stop();
    m_filterBar->clear();
    m_proxyModel->setFilterWildcard({});

    // Network folders are not watched — Qt never watches UNC paths either. Adding
    // and removing the watch is an SMB round trip on the GUI thread, which blocked
    // for seconds while a copy kept the NAS busy. F2 and finished jobs refresh them.
    m_model->setOption(QFileSystemModel::DontWatchForChanges, isOnNetworkDrive(m_currentPath));
    addServerNode(m_currentPath);
    m_model->setRootPath(m_currentPath);
    syncRootIndex();

    updateAddressBar();
    updateDriveSelection();
    updatePlaceSelection();
    updateStatusBar();
    updateEmptyHint();
    updateSizeBar();
    updateNavButtons();
    emit pathChanged(m_currentPath);
}

QString FilePanel::sanitizeFileName(const QString &name)
{
    QString result = name.trimmed();

#ifdef Q_OS_WIN
    // Strip trailing dots — Windows silently removes them anyway.
    while (!result.isEmpty() && result.endsWith('.')) {
        result.chop(1);
    }
#endif

    return result;
}

bool FilePanel::isValidEntryName(const QString &name, QString &errorString)
{
    if (name.isEmpty()) {
        errorString = tr("Name cannot be empty.");
        return false;
    }

    // "." and ".." are reserved on every filesystem (current/parent dir links).
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        errorString = tr("Names \".\" and \"..\" are reserved.");
        return false;
    }

#ifdef Q_OS_WIN
    // Windows forbidden characters: \ / : * ? " < > |
    static const QRegularExpression forbiddenChars(R"([\\/:*?"<>|])");
    const QRegularExpressionMatch match = forbiddenChars.match(name);
    if (match.hasMatch()) {
        errorString = tr("Names cannot contain '%1'.").arg(match.captured(0));
        return false;
    }

    // Control characters (0x00–0x1F) are not allowed
    for (const QChar &ch : name) {
        if (ch.unicode() < 0x20) {
            errorString = tr("Names cannot contain control characters.");
            return false;
        }
    }

    // Names must not end with a dot or space (Windows silently strips them)
    if (name.endsWith('.') || name.endsWith(' ')) {
        errorString = tr("Names must not end with a dot or space.");
        return false;
    }

    // Reserved device names (case-insensitive), with or without extension (e.g. "CON.txt")
    static const QRegularExpression reservedPattern(
        R"(^(CON|PRN|AUX|NUL|COM\d+|LPT\d+)(\..+)?$)",
        QRegularExpression::CaseInsensitiveOption);
    if (reservedPattern.match(name).hasMatch()) {
        errorString = tr("This name is reserved on Windows.");
        return false;
    }

#else
    // Linux / Unix: only '/' and null byte are forbidden
    if (name.contains('/') || name.contains(QChar(0))) {
        errorString = tr("Names cannot contain '/' or null characters.");
        return false;
    }
#endif

    errorString.clear();
    return true;
}

void FilePanel::refresh()
{
    if (isNetworkPath(m_currentPath)) {
        // Checked on the pool — a busy NAS answers a stat only after seconds.
        // Reloaded right away; a folder that is gone is left once the check returns.
        const QString path = m_currentPath;
        QtConcurrent::run(&existingFolderOrParent, path)
            .then(this, [this, path](const QString &existing) {
                if (m_currentPath == path && !pathEquals(existing, path))
                    leaveMissingFolder(existing);
            });
    } else if (const QString existing = existingFolderOrParent(m_currentPath);
               !pathEquals(existing, m_currentPath)) {
        leaveMissingFolder(existing);
        return;
    }

    // F2 or a finished job — contents may have changed; sizes of this folder,
    // everything below it and its ancestors are stale (in every tab).
    cancelFolderSizeCalculation();
    m_sizeCache->invalidate(m_currentPath);

    m_model->setRootPath({});
    m_model->setRootPath(m_currentPath);
    syncRootIndex();
    updateStatusBar();
    updateEmptyHint();
    updateSizeBar();
}

void FilePanel::leaveMissingFolder(const QString &fallback)
{
    emit statusMessage(tr("Directory no longer exists: %1").arg(m_currentPath));
    showFolder(fallback.isEmpty() ? QDir::homePath() : fallback);
}

void FilePanel::syncRootIndex()
{
    m_fileView->setRootIndex(m_proxyModel->mapFromSource(m_model->index(m_currentPath)));
}

void FilePanel::createNewFolder()
{
    const QString name = promptValidName(
        tr("New Folder"), tr("Folder name:"), tr("New Folder"),
        /*isDirectory=*/true, m_currentPath);

    if (name.isEmpty())
        return;

    const QString targetPath = QDir(m_currentPath).filePath(name);

    // not using QDir::mkdir() -> returns only bool — std::filesystem gives us the actual error message
    std::error_code ec;
    std::filesystem::create_directory(targetPath.toStdWString(), ec);
    if (ec) {
        QMessageBox::warning(this, tr("New Folder"),
                             tr("Could not create folder \"%1\":\n%2")
                                 .arg(name, QString::fromStdString(ec.message())));
        return;
    }

    refresh();

    // Select and scroll to the new folder so the user can immediately rename or open it.
    const QModelIndex srcIdx = m_model->index(targetPath);
    if (srcIdx.isValid()) {
        const QModelIndex proxyIdx = m_proxyModel->mapFromSource(srcIdx);
        if (proxyIdx.isValid()) {
            m_fileView->setCurrentIndex(proxyIdx);
            m_fileView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
        }
    }
}

void FilePanel::setShowHiddenFiles(bool visible)
{
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (visible) filters |= QDir::Hidden;
    m_model->setFilter(filters);
    refresh();
}

void FilePanel::setHidePartFiles(bool hide)
{
    m_proxyModel->setHidePartFiles(hide);
    syncRootIndex();

    // Every tab is switched at once — only the focused one writes the status bar.
    if (isActivePanel())
        updateStatusBar();
    updateEmptyHint();
    updateSizeBar();
}

void FilePanel::onItemActivated(const QModelIndex &proxyIndex)
{
    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIndex);
    const QString path = m_model->filePath(srcIdx);

    // The model already knows — no stat on the GUI thread (a busy NAS answers after seconds).
    if (m_model->isDir(srcIdx))
        navigateChecked(path);
    else
        openWithDefault(path);
}

void FilePanel::onSelectionChanged()
{
    updateStatusBar();
}

void FilePanel::onAddressBarReturn()
{
    const QString text = m_addressBar->text();
    if (isServerOnlyPath(text)) {
        updateAddressBar();   // keep showing the folder we are actually in
        openServerDialog(text);
        return;
    }
    // "\\server\share\…" and paths on mapped letters are checked in the background.
    navigateChecked(text);
}

void FilePanel::onGoBack()
{
    while (!m_backStack.isEmpty()) {
        const QString prev = m_backStack.pop();
        if (isNetworkPath(prev)) {
            showHistoryFolderChecked(prev, /*back=*/true);
            return;
        }
        if (QFileInfo(prev).isDir()) {
            m_forwardStack.push(m_currentPath);
            showFolder(prev);
            return;
        }
    }
    updateNavButtons();
}

void FilePanel::onGoForward()
{
    while (!m_forwardStack.isEmpty()) {
        const QString next = m_forwardStack.pop();
        if (isNetworkPath(next)) {
            showHistoryFolderChecked(next, /*back=*/false);
            return;
        }
        if (QFileInfo(next).isDir()) {
            m_backStack.push(m_currentPath);
            showFolder(next);
            return;
        }
    }
    updateNavButtons();
}

void FilePanel::showHistoryFolderChecked(const QString &path, bool back)
{
    // Checked on the pool — a busy NAS answers a stat only after seconds.
    // Navigating elsewhere meanwhile cancels it, like openNetworkPath().
    m_pendingNetworkPath = path;
    updateNavButtons();
    emit statusMessage(tr("Connecting to %1…").arg(QDir::toNativeSeparators(path)));

    QtConcurrent::run([path] { return QFileInfo(path).isDir(); })
        .then(this, [this, path, back](bool isDir) {
            if (m_pendingNetworkPath != path)
                return;
            m_pendingNetworkPath.clear();
            if (!isDir) {
                // Gone or unreachable — dropped like a local folder that vanished.
                updateStatusBar();
                back ? onGoBack() : onGoForward();
                return;
            }
            (back ? m_forwardStack : m_backStack).push(m_currentPath);
            showFolder(path);
        });
}

void FilePanel::onContextMenuRequested(const QPoint &pos)
{
    // Name column — renameItem() reads the name via QFileSystemModel::fileName(),
    // which returns the display text of whatever column it is given.
    const QModelIndex proxyIdx = m_fileView->indexAt(pos).siblingAtColumn(kNameColumn);
    const bool hasSelection = proxyIdx.isValid();

    QMenu menu(this);

    if (hasSelection) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        const QString path = m_model->filePath(srcIdx);

        if (m_model->isDir(srcIdx)) {
            menu.addAction(QIcon::fromTheme("folder-open"), tr("Open"), [this, path]{ navigateChecked(path); });
            menu.addAction(QIcon::fromTheme("tab-new"), tr("Open in New Tab"), [this, path]{ emit openInNewTabRequested(path); });
            menu.addAction(tr("Calculate Folder Size\tSpace"), this, &FilePanel::calculateSelectedFolderSizes);
        }
        else {
            menu.addAction(QIcon::fromTheme("document-open"), tr("Open"), [this, path]{ openWithDefault(path); });
        }

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-copy"), tr("Copy"), [this]{ copyToClipboard(false); });
        menu.addAction(QIcon::fromTheme("edit-cut"),  tr("Cut"),  [this]{ copyToClipboard(true); });

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-rename"), tr("Rename"), [this, proxyIdx]{ renameItem(proxyIdx); });

        menu.addAction(QIcon::fromTheme("edit-delete"), tr("Delete"), this, &FilePanel::deleteRequested);
        menu.addSeparator();
    }

    // Paste — always visible, disabled when clipboard has no files
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QAction *pasteAct = menu.addAction(QIcon::fromTheme("edit-paste"), tr("Paste"),
                                       this, &FilePanel::pasteRequested);
    pasteAct->setEnabled(mime && mime->hasUrls());

    menu.addSeparator();
    menu.addAction(QIcon::fromTheme("folder-new"),   tr("New Folder\tF7"), this, &FilePanel::createNewFolder);
    menu.addAction(QIcon::fromTheme("view-refresh"), tr("Refresh\tF2"),    this, &FilePanel::refresh);
    menu.addAction(QIcon::fromTheme("utilities-terminal"), tr("Open Terminal Here	F9"),
                   this, [this]{ openTerminal(m_currentPath); });

    menu.addSeparator();
    QAction *calcAllAct = menu.addAction(tr("Calculate All Folder Sizes\tAlt+Shift+Enter"),
                                         this, &FilePanel::calculateAllFolderSizes);
    calcAllAct->setEnabled(!uncalculatedFolders().isEmpty());
    addFolderPlacementActions(menu.addMenu(tr("Folders")));

    menu.exec(m_fileView->viewport()->mapToGlobal(pos));
}


void FilePanel::updateAddressBar()
{
    m_addressBar->setText(m_currentPath);
}

void FilePanel::updateStatusBar()
{
    // Use the proxy so the count reflects the active filter, not the raw FS.
    const QModelIndexList rows         = visibleRows();
    const QModelIndexList selectedRows = m_fileView->selectionModel()->selectedRows();
    const SizeTotal       folderTotal  = sizeTotal(rows);

    // "42 items (3.8 GB)" — the size part is left out while nothing is known.
    const auto countText = [](const QString &text, const SizeTotal &total) {
        const QString size = formatSizeTotal(total);
        return size.isEmpty() ? text : QString("%1 (%2)").arg(text, size);
    };

    const QString itemsText = countText(tr("%1 items").arg(rows.size()), folderTotal);
    const QString msg = selectedRows.isEmpty()
        ? QString("%1  |  %2").arg(itemsText, m_currentPath)
        : QString("%1  |  %2  |  %3")
              .arg(itemsText,
                   countText(tr("%1 selected").arg(selectedRows.size()), sizeTotal(selectedRows)),
                   m_currentPath);

    emit statusMessage(msg);
}

QModelIndexList FilePanel::visibleRows() const
{
    QModelIndexList rows;
    const QModelIndex root = m_fileView->rootIndex();
    const int count = m_proxyModel->rowCount(root);
    rows.reserve(count);
    for (int row = 0; row < count; ++row)
        rows << m_proxyModel->index(row, kNameColumn, root);
    return rows;
}

FilePanel::SizeTotal FilePanel::sizeTotal(const QModelIndexList &rows) const
{
    SizeTotal total;
    for (const QModelIndex &proxyIdx : rows) {
        const qint64 bytes = m_proxyModel->sizeBytes(proxyIdx);
        if (bytes < 0)
            total.complete = false;   // folder without a calculated size
        else
            total.bytes += bytes;
    }
    return total;
}

QString FilePanel::formatSizeTotal(const SizeTotal &total)
{
    if (total.complete)
        return formatSize(total.bytes);
    // Only part of it is known — say so instead of showing a number that is too small.
    return total.bytes > 0 ? QStringLiteral("≥ ") + formatSize(total.bytes) : QString();
}

bool FilePanel::isActivePanel() const
{
    return m_fileView->property("panelActive").toBool();
}

void FilePanel::updateNavButtons()
{
    m_backButton->setEnabled(!m_backStack.isEmpty());
    m_forwardButton->setEnabled(!m_forwardStack.isEmpty());
}

void FilePanel::renameItem(const QModelIndex &proxyIdx)
{
    const QModelIndex srcIdx  = m_proxyModel->mapToSource(proxyIdx);
    const QString currentName = m_model->fileName(srcIdx);
    const QString currentPath = m_model->filePath(srcIdx);
    const QFileInfo fileInfo(currentPath);

    const QString name = promptValidName(
        tr("Rename"), tr("New name:"), currentName,
        m_model->isDir(srcIdx), fileInfo.absolutePath(), currentPath);

    if (name.isEmpty() || name == currentName)
        return;

    const QString targetPath = fileInfo.dir().filePath(name);

    QFile file(currentPath);
    if (!file.rename(targetPath))
        QMessageBox::warning(this, tr("Rename failed"), file.errorString());
}

void FilePanel::setActive(bool active)
{
    if (m_fileView->property("panelActive").toBool() == active)
        return;  // no-op — avoids the polish/unpolish cycle entirely

    m_fileView->setProperty("panelActive", active);
    // Re-evaluate the existing (already-parsed) stylesheet against the new
    // property value; cheaper than setStyleSheet() because no QSS re-parse.
    m_fileView->style()->unpolish(m_fileView);
    m_fileView->style()->polish(m_fileView);

    if (active)
        updateStatusBar();
}

void FilePanel::openWithDefault(const QString &path)
{
    const QUrl fileUrl = QUrl::fromLocalFile(path);

    if (QDesktopServices::openUrl(fileUrl))
        return;

    const QString fileName = QFileInfo(path).fileName();
    const QString message  = tr("Could not open \"%1\"").arg(fileName);

    QMessageBox::warning(this, tr("Open failed"), message);
}

void FilePanel::openTerminal(const QString &dir)
{
    const QString nativeDir = QDir::toNativeSeparators(dir);
    bool started = false;

#ifdef Q_OS_WIN
    // Windows Terminal if installed (its default profile, usually PowerShell),
    // otherwise PowerShell. Not cmd.exe — it cannot start in a UNC folder and
    // silently falls back to C:\Windows.
    const QString windowsTerminal = QStandardPaths::findExecutable(QStringLiteral("wt"));
    if (!windowsTerminal.isEmpty()) {
        // wt reads ";" as its command separator — escape it in the path.
        QString wtDir = nativeDir;
        wtDir.replace(QLatin1Char(';'), QLatin1String("\\;"));
        started = QProcess::startDetached(windowsTerminal, {QStringLiteral("-d"), wtDir});
    }
    // Detached processes get their own console window on Windows.
    if (!started)
        started = QProcess::startDetached(QStringLiteral("powershell.exe"), {QStringLiteral("-NoExit")}, dir);
#else
    // TODO(Linux): only the Debian alternatives link — add more terminals when porting.
    started = QProcess::startDetached(QStringLiteral("x-terminal-emulator"), {}, dir);
#endif

    if (!started)
        QMessageBox::warning(this, tr("Open Terminal"),
                             tr("Could not open a terminal in \"%1\".").arg(nativeDir));
}

void FilePanel::saveSettings(QSettings &settings) const
{
    // A network folder still being opened is where the user wants to be — a NAS
    // that was offline at startup must not lose the tab's folder.
    settings.setValue(QStringLiteral("Path"),
                      m_pendingNetworkPath.isEmpty() ? m_currentPath : m_pendingNetworkPath);

    const QHeaderView *header = m_fileView->header();
    settings.setValue(QStringLiteral("SortColumn"), columnKey(header->sortIndicatorSection()));
    settings.setValue(QStringLiteral("SortOrder"),
                      header->sortIndicatorOrder() == Qt::DescendingOrder ? QStringLiteral("descending")
                                                                          : QStringLiteral("ascending"));
    settings.setValue(QStringLiteral("Folders"), folderPlacementKey(m_proxyModel->folderPlacement()));

    // The Name column stretches — only the others have a width of their own.
    QStringList widths;
    for (const int column : {kSizeColumn, kTypeColumn, kDateColumn})
        widths << QString::number(header->sectionSize(column));
    settings.setValue(QStringLiteral("ColumnWidths"), widths);
}

void FilePanel::restoreSettings(const QSettings &settings)
{
    const int column = columnFromKey(settings.value(QStringLiteral("SortColumn")).toString());
    const bool descending = settings.value(QStringLiteral("SortOrder")).toString() == QLatin1String("descending");
    if (column >= 0)
        m_fileView->sortByColumn(column, descending ? Qt::DescendingOrder : Qt::AscendingOrder);

    setFolderPlacement(folderPlacementFromKey(settings.value(QStringLiteral("Folders")).toString()));

    const QStringList widths = settings.value(QStringLiteral("ColumnWidths")).toStringList();
    const int columns[] = {kSizeColumn, kTypeColumn, kDateColumn};
    for (int i = 0; i < widths.size() && i < int(std::size(columns)); ++i) {
        bool ok = false;
        const int width = widths.at(i).toInt(&ok);
        if (ok && width > 0)
            m_fileView->header()->resizeSection(columns[i], width);
    }

    restoreFolder(settings.value(QStringLiteral("Path")).toString());
}

void FilePanel::restoreFolder(const QString &path)
{
    const QString cleanPath = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if (cleanPath.isEmpty() || pathEquals(cleanPath, m_currentPath))
        return;

    // Local folders are checked right away — the window is not shown yet.
    if (!isOnNetworkDrive(cleanPath)) {
        if (const QString existing = existingFolderOrParent(cleanPath); !existing.isEmpty())
            showFolder(existing);
        return;
    }

    // The NAS may be asleep or gone — a stat on the GUI thread would freeze the
    // window for seconds. The tab shows its start folder until the check returns;
    // navigating elsewhere first cancels the restore (clears m_pendingNetworkPath).
    m_pendingNetworkPath = cleanPath;
    emit statusMessage(tr("Connecting to %1…").arg(QDir::toNativeSeparators(cleanPath)));

    QtConcurrent::run(&existingFolderOrParent, cleanPath)
        .then(this, [this, cleanPath](const QString &existing) {
            if (m_pendingNetworkPath != cleanPath)
                return;   // user navigated elsewhere in the meantime
            m_pendingNetworkPath.clear();
            if (existing.isEmpty()) {
                updateStatusBar();
                return;
            }

            // Keep the share in the dropdown for the session, like finishNetworkOpen().
            if (isSharePath(existing))
                emit networkSharesFound({uncShareRoot(existing)});
            showFolder(existing);
        });
}

bool FilePanel::handleMiddleClickNavigation(QMouseEvent *mouseEvent)
{
    const QModelIndex proxyIdx = m_fileView->indexAt(mouseEvent->pos());
    if (!proxyIdx.isValid())
        return false;

    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    if (!srcIdx.isValid())
        return false;

    if (!m_model->isDir(srcIdx))
        return false;

    const QString path = m_model->filePath(srcIdx);

    emit openInNewTabRequested(path);
    return true;
}

void FilePanel::setDrives(const QList<DriveInfo> &drives)
{
    m_drives = drives;
    rebuildDriveCombo();
}

void FilePanel::setNetworkSearchRunning(bool running)
{
    if (m_networkSearchRunning == running)
        return;
    m_networkSearchRunning = running;
    rebuildDriveCombo();
}

void FilePanel::rebuildDriveCombo()
{
    // The model's provider returns the native shell icon per drive type
    // (HDD, USB, DVD) on Windows.
    const QAbstractFileIconProvider *iconProvider = m_model->iconProvider();
    auto *itemModel = qobject_cast<QStandardItemModel *>(m_driveCombo->model());

    // Block signals while rebuilding so no listener sees transient states.
    const QSignalBlocker blocker(m_driveCombo);
    m_driveCombo->clear();

    for (const DriveInfo &drive : std::as_const(m_drives)) {
        const QString label = buildDriveLabel(drive);
        if (label.isEmpty())
            continue;

        // Unprobed mapped drives have no volume name yet — show the share instead.
        QString description = drive.volumeName;
        if (description.isEmpty() && drive.kind == DriveInfo::Kind::MappedDrive)
            description = drive.remotePath;
        QString text = description.isEmpty()
                           ? label
                           : QString("%1  %2").arg(label, description);
        if (drive.state == DriveInfo::State::Offline)
            text += tr("  (offline)");

        // Network drives use the generic type icon — the shell lookup for a
        // path on an unreachable server would block the GUI thread.
        QIcon icon;
        if (iconProvider) {
            icon = drive.isNetwork()
                       ? iconProvider->icon(QAbstractFileIconProvider::Network)
                       : iconProvider->icon(QFileInfo(drive.rootPath));
        }

        m_driveCombo->addItem(icon, text, drive.rootPath);
        const int row = m_driveCombo->count() - 1;
        m_driveCombo->setItemData(row, buildDriveTooltip(drive), Qt::ToolTipRole);

        // Offline drives stay visible but cannot be picked — navigating there
        // would block on the network timeout.
        if (drive.state == DriveInfo::State::Offline && itemModel)
            itemModel->item(row)->setEnabled(false);
    }

    // Action entries at the end — no root, so updateDriveSelection() never selects them.
    const auto addAction = [this](DriveAction action, const QIcon &icon,
                                  const QString &text, const QString &tooltip) {
        m_driveCombo->addItem(icon, text);
        const int row = m_driveCombo->count() - 1;
        m_driveCombo->setItemData(row, int(action), kDriveActionRole);
        m_driveCombo->setItemData(row, tooltip, Qt::ToolTipRole);
        return row;
    };

    m_driveCombo->insertSeparator(m_driveCombo->count());
    const int searchRow = addAction(DriveAction::FindNetwork,
                                    style()->standardIcon(QStyle::SP_BrowserReload),
                                    m_networkSearchRunning ? tr("Searching network…")
                                                           : tr("Find network drives…"),
                                    tr("Search the LAN for NAS / SMB servers and list their shares"));
    if (m_networkSearchRunning && itemModel)
        itemModel->item(searchRow)->setEnabled(false);

    addAction(DriveAction::ConnectServer,
              style()->standardIcon(QStyle::SP_DriveNetIcon),
              tr("Connect to server…"),
              tr("Enter a server name like \\\\raspberrypi and pick one of its shares"));

    updateDriveSelection();
}

void FilePanel::updateDriveSelection()
{
    // Longest matching root wins — handles mount points nested inside a drive.
    int bestIndex  = -1;
    int bestLength = 0;
    for (int i = 0; i < m_driveCombo->count(); ++i) {
        const QString root = QDir::cleanPath(m_driveCombo->itemData(i).toString());
        if (root.isEmpty())
            continue;   // separator / "Find network drives…" entry
        if (isSameOrInside(m_currentPath, root) && root.length() > bestLength) {
            bestIndex  = i;
            bestLength = root.length();
        }
    }
    m_driveCombo->setCurrentIndex(bestIndex);
}

void FilePanel::onDriveActivated(int index)
{
    switch (DriveAction(m_driveCombo->itemData(index, kDriveActionRole).toInt())) {
    case DriveAction::FindNetwork:
        emit networkSearchRequested();
        updateDriveSelection();
        // Re-open the list so found shares can be watched appearing live.
        QTimer::singleShot(0, m_driveCombo, &QComboBox::showPopup);
        return;
    case DriveAction::ConnectServer:
        updateDriveSelection();
        // Deferred — let the popup finish closing before the modal dialog opens.
        QTimer::singleShot(0, this, [this] { openServerDialog(); });
        return;
    case DriveAction::None:
        break;
    }

    // Network drives — with or without a letter — are checked in the background:
    // the first access sets up the connection and may need a sign-in.
    const QString root = m_driveCombo->itemData(index).toString();
    if (!root.isEmpty())
        navigateChecked(root);

    // navigateTo() may refuse (drive vanished) — snap back to the actual drive.
    updateDriveSelection();
    m_fileView->setFocus();
}

void FilePanel::openServerDialog(const QString &initialServer)
{
    const QAbstractFileIconProvider *iconProvider = m_model->iconProvider();
    const QIcon shareIcon = iconProvider ? iconProvider->icon(QAbstractFileIconProvider::Network)
                                         : style()->standardIcon(QStyle::SP_DriveNetIcon);

    ConnectServerDialog dlg(initialServer, shareIcon, this);
    connect(&dlg, &ConnectServerDialog::sharesFound, this, &FilePanel::networkSharesFound);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString share = dlg.selectedShare();
    if (!share.isEmpty())
        openNetworkPath(share);
}

void FilePanel::openNetworkPath(const QString &path)
{
    const QString target = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));

    // root:      what has to answer for the target to be reachable ("//nas/media" or "Z:/").
    // shareRoot: the share behind it — server probe, share listing and sign-in
    //            use it. A mapped letter gets it from its UNC target.
    QString root;
    QString shareRoot;
    if (isSharePath(target)) {
        root = shareRoot = uncShareRoot(target);
    } else if (const DriveInfo *drive = mappedDriveFor(target)) {
        root      = drive->rootPath;
        shareRoot = uncShareRoot(drive->remotePath);
    } else {
        return;   // "\\server" alone or not a network path — callers handle those
    }
    const QString server = shareRoot.section('/', 2, 2);   // "//server/share" → "server"

    m_pendingNetworkPath = target;
    emit statusMessage(tr("Connecting to %1…").arg(QDir::toNativeSeparators(target)));

    // All checks run off the GUI thread — the first SMB access can take ~10 s.
    QtConcurrent::run(&checkNetworkPath, target, root, shareRoot, server)
        .then(this, [this, target, root, shareRoot, server](const ReachCheck &check) {
            if (m_pendingNetworkPath != target)
                return;   // user navigated elsewhere in the meantime

            switch (check.reach) {
            case Reach::Ok:
                finishNetworkOpen(target, shareRoot, /*checked=*/true);
                return;
            case Reach::FolderMissing:
                abortNetworkOpen(tr("Path does not exist: %1").arg(QDir::toNativeSeparators(target)));
                return;
            case Reach::DriveUnreachable:
                abortNetworkOpen(tr("Drive %1 is not reachable.").arg(QDir::toNativeSeparators(root)));
                return;
            case Reach::ShareMissing:
                abortNetworkOpen(tr("Share \"%1\" not found on %2.")
                                     .arg(QDir::toNativeSeparators(shareRoot), server));
                return;
            case Reach::ServerFailed:
                abortNetworkOpen(NetworkDiscovery::describeServerState(check.serverState, server));
                return;
            case Reach::LoginRequired:
                signInToNetworkPath(target, shareRoot);
                return;
            }
        });
}

void FilePanel::signInToNetworkPath(const QString &target, const QString &shareRoot)
{
    // Same as Explorer: Windows asks for credentials — only now, for a real server.
    // Returns once the dialog is closed; the connect runs on the pool. The path
    // stays pending meanwhile — navigating elsewhere cancels the open.
    NetworkDiscovery::signIn(this, shareRoot)
        .then(this, [this, target, shareRoot](const NetworkDiscovery::SignInOutcome &outcome) {
            if (m_pendingNetworkPath != target)
                return;   // user navigated elsewhere while signing in

            switch (outcome.result) {
            case NetworkDiscovery::SignInResult::Connected:
                finishNetworkOpen(target, shareRoot, /*checked=*/false);
                return;
            case NetworkDiscovery::SignInResult::Cancelled:
                abortNetworkOpen({});
                return;
            case NetworkDiscovery::SignInResult::Failed:
                abortNetworkOpen(tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(target),
                                                              outcome.error));
                return;
            }
        });
}

void FilePanel::finishNetworkOpen(const QString &target, const QString &shareRoot, bool checked)
{
    // Keep the share in the dropdown for the session — a mapped letter is already there.
    if (isSharePath(target))
        emit networkSharesFound({shareRoot});
    // Server root stat'ed once per session — see serverRoots().
    const bool statServer = isSharePath(target) && !serverRoots().contains(pathKey(serverOf(target)));
    if (checked && !statServer) {
        enterFolder(target);   // clears m_pendingNetworkPath
        return;
    }

    // After a sign-in the folder itself is still unchecked. That is the first
    // access to the share (tree connect, disk may wake up) — on the GUI thread
    // it froze the window ("Not Responding"). The path stays pending meanwhile.
    QtConcurrent::run([target, checked, statServer] {
        const bool isDir = checked || QFileInfo(target).isDir();
        return std::make_pair(isDir, isDir && statServer ? statServerRoot(target) : QFileInfo());
    })
        .then(this, [this, target](const std::pair<bool, QFileInfo> &result) {
            if (m_pendingNetworkPath != target)
                return;   // user navigated elsewhere in the meantime
            if (!result.first) {
                abortNetworkOpen(tr("Path does not exist: %1").arg(QDir::toNativeSeparators(target)));
                return;
            }
            if (!result.second.filePath().isEmpty())
                serverRoots().insert(pathKey(serverOf(target)), result.second);
            enterFolder(target);
        });
}

void FilePanel::addServerNode(const QString &path)
{
    if (!isSharePath(path))
        return;
    const QString server = serverOf(path);
    const auto known = serverRoots().constFind(pathKey(server));
    if (known == serverRoots().cend())
        return;   // never opened via openNetworkPath() — Qt stats it itself

    // What QFileSystemModelPrivate::node() does for a new host, minus its stat
    // and the volumeName() shell lookup (SHCreateItemFromParsingName on the
    // server — network I/O as well). Children keys compare case-insensitively.
    auto *d = static_cast<QFileSystemModelPrivate *>(QObjectPrivate::get(m_model));
    const QString host = QStringLiteral("\\\\") + server;
    if (d->root.children.contains(host))
        return;
    auto *node = new QFileSystemModelPrivate::QFileSystemNode(host, &d->root);
    node->populate(*known);
    d->root.children.insert(host, node);
    d->addVisibleFiles(&d->root, {host});
}

void FilePanel::abortNetworkOpen(const QString &message)
{
    m_pendingNetworkPath.clear();
    if (!message.isEmpty())
        QMessageBox::critical(this, tr("Error"), message);
    updateStatusBar();
}

void FilePanel::setupPlaceButtons(QHBoxLayout *layout)
{
    // Same set and order as Explorer's "This PC", plus the user's home folder.
    static constexpr QStandardPaths::StandardLocation kPlaces[] = {
        QStandardPaths::DesktopLocation,
        QStandardPaths::DocumentsLocation,
        QStandardPaths::DownloadLocation,
        QStandardPaths::PicturesLocation,
        QStandardPaths::MusicLocation,
        QStandardPaths::MoviesLocation,
        QStandardPaths::HomeLocation,
    };

    for (const auto location : kPlaces) {
        if (auto *button = createPlaceButton(location)) {
            m_placeButtons.append(button);
            layout->addWidget(button);
        }
    }
}

QToolButton *FilePanel::createPlaceButton(QStandardPaths::StandardLocation location)
{
    // Qt resolves these via SHGetKnownFolderPath on Windows, so OneDrive
    // redirection and relocated folders are already handled.
    const QString path = QDir::cleanPath(QStandardPaths::writableLocation(location));
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return nullptr;

    // Missing folders fall back to Documents — skip duplicates.
    for (const QToolButton *existing : std::as_const(m_placeButtons)) {
        if (existing->property("placePath").toString().compare(path, Qt::CaseInsensitive) == 0)
            return nullptr;
    }

    const QAbstractFileIconProvider *iconProvider = m_model->iconProvider();
    const QIcon icon = iconProvider
                           ? iconProvider->icon(QFileInfo(path))
                           : style()->standardIcon(QStyle::SP_DirIcon);

    // Home has no meaningful display name — show the user name instead.
    const QString name = location == QStandardPaths::HomeLocation
                             ? QDir(path).dirName()
                             : QStandardPaths::displayName(location);

    auto *button = new QToolButton(this);
    button->setIcon(icon);
    button->setToolTip(QString("%1 – %2").arg(name, QDir::toNativeSeparators(path)));
    button->setFixedSize(kNavButtonSize, kNavButtonSize);
    button->setAutoRaise(true);
    button->setCheckable(true);
    // Keyboard focus stays in the list view — Tab switches panels, not widgets.
    button->setFocusPolicy(Qt::NoFocus);
    button->setProperty("placePath", path);

    connect(button, &QToolButton::clicked, this, [this, path] {
        navigateChecked(path);   // a user folder may be redirected to a network share
        // clicked() toggled the check state — re-sync with the actual path.
        updatePlaceSelection();
        m_fileView->setFocus();
    });

    return button;
}

void FilePanel::updatePlaceSelection()
{
    // Exact match only — subfolders do not highlight their parent place.
    for (QToolButton *button : std::as_const(m_placeButtons)) {
        const QString placePath = button->property("placePath").toString();
        button->setChecked(m_currentPath.compare(placePath, Qt::CaseInsensitive) == 0);
    }
}

void FilePanel::onFilterChanged(const QString & /*text*/)
{
    // Debounced — keystrokes restart the timer; applyFilter() runs once it idles.
    m_filterDebounceTimer->start();
}

void FilePanel::applyFilter()
{
    const QString trimmedText = m_filterBar->text().trimmed();

    if (trimmedText.isEmpty()) {
        m_proxyModel->setFilterWildcard({});
    } else {
        QString pattern = trimmedText;
        if (!pattern.contains('*') && !pattern.contains('?'))
            pattern += '*';  // no wildcard → "starts with" (use *foo for "contains")
        m_proxyModel->setFilterWildcard(pattern);
    }

    syncRootIndex();

    // Select the first visible item so arrow-key navigation works immediately
    const QModelIndex first = m_proxyModel->index(0, kNameColumn, m_fileView->rootIndex());
    if (first.isValid()) {
        m_fileView->setCurrentIndex(first);
        m_fileView->scrollTo(first);
    }

    updateEmptyHint();
}

void FilePanel::updateEmptyHint()
{
    // Rows shown: no hint, and no need to ask the filesystem.
    m_emptyHint->hide();
    if (m_proxyModel->rowCount(m_fileView->rootIndex()) > 0)
        return;

    // Nothing shown: empty folder, a filter that matches nothing, or still loading.
    // Ask the filesystem, not the proxy: QFileSystemModel loads asynchronously,
    // so rowCount() is 0 while a non-empty folder is still being read.
    // isEmpty() stops at the first entry and honours the hidden-files setting —
    // but on a network share even that listing can take seconds, so off the GUI thread.
    const QString       path   = m_currentPath;
    const QDir::Filters filter = m_model->filter();
    QtConcurrent::run([path, filter] { return QDir(path).isEmpty(filter); })
        .then(this, [this, path](bool folderEmpty) {
            // Another folder by now, or rows arrived — a newer call decides.
            if (path != m_currentPath || m_proxyModel->rowCount(m_fileView->rootIndex()) > 0)
                return;

            const bool filterActive = !m_filterBar->text().trimmed().isEmpty();
            // Not empty, no filter, nothing shown — only hidden .part files left.
            const bool onlyPartFiles = !folderEmpty && !filterActive && m_proxyModel->hidesPartFiles();
            if (folderEmpty)
                m_emptyHint->setText(tr("This folder is empty."));
            else if (filterActive)
                m_emptyHint->setText(tr("No items match the filter."));
            else if (onlyPartFiles)
                m_emptyHint->setText(tr("Only hidden .part files in this folder."));

            m_emptyHint->setVisible(folderEmpty || filterActive || onlyPartFiles);
        });
}

void FilePanel::calculateSelectedFolderSizes()
{
    QModelIndexList rows = m_fileView->selectionModel()->selectedRows();
    if (rows.isEmpty() && m_fileView->currentIndex().isValid())
        rows << m_fileView->currentIndex();

    QStringList folders;
    for (const QModelIndex &proxyIdx : std::as_const(rows)) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        if (m_model->isDir(srcIdx))
            folders << m_model->filePath(srcIdx);
    }

    // Explicit request — recalculate even folders that already have a size.
    for (const QString &folder : std::as_const(folders))
        m_sizeCache->remove(folder);
    startFolderSizeCalculation(folders);
}

void FilePanel::calculateAllFolderSizes()
{
    startFolderSizeCalculation(uncalculatedFolders());
}

QStringList FilePanel::uncalculatedFolders() const
{
    // Visible rows only — with an active filter, "all folders" means the matching ones.
    QStringList folders;
    for (const QModelIndex &proxyIdx : visibleRows()) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        if (!m_model->isDir(srcIdx))
            continue;
        const QString path = m_model->filePath(srcIdx);
        if (!m_sizeCache->contains(path) && !m_proxyModel->isDirPending(path))
            folders << path;
    }
    return folders;
}

void FilePanel::startFolderSizeCalculation(const QStringList &folders)
{
    // A running QtConcurrent task cannot take more work — restart it with its
    // unfinished folders plus the new ones.
    QStringList todo = isCalculatingFolderSizes() ? m_proxyModel->pendingDirs() : QStringList();
    for (const QString &folder : folders) {
        if (!todo.contains(folder))
            todo << folder;
    }
    if (todo.isEmpty())
        return;

    m_sizeWatcher->cancel();
    m_proxyModel->setPendingDirs(todo);
    m_sizeDone       = 0;
    m_sizeTotal      = int(todo.size());
    // Own counter per run — a cancelled run may still count for a moment.
    m_sizeBytesSoFar = std::make_shared<std::atomic<qint64>>(0);

    // A directory walk can take minutes on a network drive — never on the GUI thread.
    m_sizeWatcher->setFuture(QtConcurrent::run(&FolderSizeCache::calculate, todo, m_sizeBytesSoFar));
    m_sizeProgressTimer->start();
    m_fileView->viewport()->update();
    updateSizeBar();
}

void FilePanel::cancelFolderSizeCalculation()
{
    if (!isCalculatingFolderSizes())
        return;

    // The pool thread checks for cancellation at every directory entry.
    m_sizeWatcher->cancel();
    m_sizeProgressTimer->stop();
    m_proxyModel->clearPendingDirs();
    m_fileView->viewport()->update();
    updateSizeBar();
}

bool FilePanel::isCalculatingFolderSizes() const
{
    return m_sizeWatcher->isRunning() && !m_sizeWatcher->isCanceled();
}

void FilePanel::onFolderSizeReady(int resultIndex)
{
    // The requested folder comes last, after all its subfolders (post-order walk).
    const QVector<FolderSize> sizes = m_sizeWatcher->resultAt(resultIndex);
    if (!sizes.isEmpty())
        m_proxyModel->removePendingDir(sizes.last().path);
    ++m_sizeDone;
    // changed() → scheduleSizeRefresh() in every tab, this one included.
    m_sizeCache->insert(sizes);
}

void FilePanel::onFolderSizeCalculationFinished()
{
    m_sizeProgressTimer->stop();
    m_proxyModel->clearPendingDirs();
    m_fileView->viewport()->update();
    updateSizeBar();
}

void FilePanel::scheduleSizeRefresh()
{
    m_fileView->viewport()->update();
    // Re-sort and re-total at most every kSizeRefreshDelayMs — doing it per
    // result would re-sort the whole list for each folder.
    if (!m_sizeRefreshTimer->isActive())
        m_sizeRefreshTimer->start();
}

void FilePanel::applySizeChanges()
{
    if (m_fileView->header()->sortIndicatorSection() == kSizeColumn)
        m_proxyModel->invalidate();

    // Only the focused panel writes the window's status bar.
    if (isActivePanel())
        updateStatusBar();

    updateSizeBar();
    m_fileView->viewport()->update();
}

void FilePanel::updateSizeBar()
{
    const int missingAll = int(uncalculatedFolders().size());
    m_calcSizesButton->setEnabled(missingAll > 0);

    if (isCalculatingFolderSizes()) {
        m_sizeBarLabel->setText(tr("Calculating folder sizes… %1 / %2  —  %3")
                                    .arg(m_sizeDone)
                                    .arg(m_sizeTotal)
                                    .arg(formatSize(m_sizeBytesSoFar->load())));
        m_sizeBarCalcButton->hide();
        m_sizeBarCloseButton->hide();
        m_sizeBarCancelButton->show();
        m_sizeBar->show();
        return;
    }

    // Offered only where it matters: sorted by size, with folders still unknown.
    const bool sortedBySize = m_fileView->header()->sortIndicatorSection() == kSizeColumn;
    const int  missing      = sortedBySize && !m_sizeBarDismissed ? missingAll : 0;
    if (missing == 0) {
        m_sizeBar->hide();
        return;
    }

    QString text = tr("%n folder(s) without size.", nullptr, missing);
    // Say up front that it can take long — each folder is a round trip over SMB.
    if (isNetworkPath(m_currentPath))
        text += QLatin1Char(' ') + tr("Network drive — this may take a while.");
    m_sizeBarLabel->setText(text);
    m_sizeBarCalcButton->show();
    m_sizeBarCloseButton->show();
    m_sizeBarCancelButton->hide();
    m_sizeBar->show();
}

const DriveInfo *FilePanel::mappedDriveFor(const QString &path) const
{
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    // A bare "Z:" typed into the address bar means the drive root "Z:/".
    if (cleaned.size() == 2 && cleaned.at(1) == QLatin1Char(':'))
        cleaned += QLatin1Char('/');

    // Answered from the last drive scan, no filesystem access.
    for (const DriveInfo &drive : std::as_const(m_drives)) {
        if (drive.kind == DriveInfo::Kind::MappedDrive && isSameOrInside(cleaned, drive.rootPath))
            return &drive;
    }
    return nullptr;
}

bool FilePanel::isNetworkPath(const QString &path) const
{
    const QString cleaned = QDir::fromNativeSeparators(path.trimmed());
    return cleaned.startsWith(QLatin1String("//")) || mappedDriveFor(cleaned);
}

void FilePanel::navigateChecked(const QString &path)
{
    if (isNetworkPath(path))
        openNetworkPath(path);
    else
        navigateTo(path);
}

// The analyzer does not know Qt's parent ownership: the group belongs to @p menu
// and dies with it (the placement button's menu deletes it on every rebuild).
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void FilePanel::addFolderPlacementActions(QMenu *menu)
{
    auto *group = new QActionGroup(menu);
    const auto addPlacement = [this, menu, group](FolderPlacement placement, const QString &text) {
        QAction *action = menu->addAction(text, this, [this, placement] { setFolderPlacement(placement); });
        action->setCheckable(true);
        action->setChecked(m_proxyModel->folderPlacement() == placement);
        group->addAction(action);
    };

    for (const FolderPlacement placement : {FolderPlacement::Top, FolderPlacement::Bottom,
                                            FolderPlacement::Mixed})
        addPlacement(placement, folderPlacementText(placement));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void FilePanel::setFolderPlacement(FolderPlacement placement)
{
    // Per tab — the proxy belongs to this panel; saved with the tab (saveSettings()).
    m_proxyModel->setFolderPlacement(placement);
    m_proxyModel->invalidate();
    m_placementButton->setText(folderPlacementText(placement));
}

void FilePanel::copyToClipboard(bool isCut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) return;

    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &filePath : paths)
        urls << QUrl::fromLocalFile(filePath);

    mime->setUrls(urls);

    if (isCut)
        ClipboardUtils::writeCutFlag(mime);

    QApplication::clipboard()->setMimeData(mime);
}

QString FilePanel::buildDriveLabel(const DriveInfo &drive) const
{
    const QString rootPath = QDir::toNativeSeparators(drive.rootPath);

    if (rootPath.length() >= 2 && rootPath[1] == ':')
        return rootPath.left(2).toUpper();          // "C:", "D:", …

    if (drive.kind == DriveInfo::Kind::NetworkShare)
        return drive.remotePath;                    // "\\raspberrypi\pi-ssd"

    return drive.displayName.left(5);
}

QString FilePanel::buildDriveTooltip(const DriveInfo &drive) const
{
    QString tooltip = QDir::toNativeSeparators(drive.rootPath);

    if (!drive.volumeName.isEmpty())
        tooltip += QString(" (%1)").arg(drive.volumeName);

    if (!drive.remotePath.isEmpty() && drive.kind == DriveInfo::Kind::MappedDrive)
        tooltip += "\n" + drive.remotePath;

    if (drive.state == DriveInfo::State::Offline)
        tooltip += "\n" + tr("Offline — server not reachable");

    if (!drive.fileSystemType.isEmpty())
        tooltip += "\n" + drive.fileSystemType;

    if (drive.bytesTotal > 0)
        tooltip += "\n" + tr("%1 free of %2").arg(formatSize(drive.bytesAvailable),
                                                  formatSize(drive.bytesTotal));

    return tooltip;
}

QString FilePanel::promptValidName(const QString &title, const QString &label, const QString &suggestion, bool isDirectory, const QString &parentDir, const QString &excludePath)
{
    QString lastSuggestion = suggestion;

    while (true) {
        bool ok = false;
        QString name = QInputDialog::getText(
            this, title, label, QLineEdit::Normal, lastSuggestion, &ok);

        if (!ok || name.trimmed().isEmpty())
            return {};  // user cancelled

        name = sanitizeFileName(name);

        QString errorString;
        if (!isValidEntryName(name, errorString)) {
            QMessageBox::warning(this, title, errorString);
            lastSuggestion = name;
            continue;
        }

        const QString targetPath = QDir(parentDir).filePath(name);
        if (QFileInfo::exists(targetPath)) {
            // On rename the target path is the item itself — canonicalFilePath
            // resolves Windows case-insensitive collisions to the same file, so
            // the user can keep the name unchanged or only flip case.
            const QString targetCanon = QFileInfo(targetPath).canonicalFilePath();
            const QString excludeCanon = excludePath.isEmpty()
                                             ? QString()
                                             : QFileInfo(excludePath).canonicalFilePath();
            const bool sameFile = !targetCanon.isEmpty() && targetCanon == excludeCanon;
            if (!sameFile) {
                QMessageBox::warning(this, title,
                                     isDirectory ? tr("A folder with this name already exists.")
                                                 : tr("A file or folder with this name already exists."));
                lastSuggestion = name;
                continue;
            }
        }

        return name;
    }
}

bool FilePanel::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_filterBar && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down ||
            key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
            QApplication::sendEvent(m_fileView, event);
            return true;
        }
        // Enter: filter now and hand the focus back to the list — the next Enter
        // opens the selected entry. A filter already applied is not applied again:
        // that would reset the row picked with the arrow keys to the first one.
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (m_filterDebounceTimer->isActive()) {
                m_filterDebounceTimer->stop();
                applyFilter();
            }
            m_fileView->setFocus();
            return true;
        }
    }

    if (obj == m_fileView->viewport() && event->type() == QEvent::Resize) {
        const QWidget *viewport = m_fileView->viewport();
        // Height from font metrics, not sizeHint() — the text may still be empty here.
        m_emptyHint->setGeometry(0, kEmptyHintTopMargin,
                                 viewport->width(), m_emptyHint->fontMetrics().height() * 2);
    }

    if (obj == m_fileView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        if (mouseEvent->button() == Qt::MiddleButton) {
            if (handleMiddleClickNavigation(mouseEvent))
                return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
