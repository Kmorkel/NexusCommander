#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QClipboard>
#include <QApplication>
#include <QShortcut>
#include <QStorageInfo>
#include <QScrollArea>
#include <QFrame>
#include <QStyle>
#include "clipboardUtils.h"
#include "filePanel.h"

namespace {
constexpr int kNavButtonSize       = 28;
constexpr int kDriveBarHeight      = 28;
constexpr int kDriveButtonMinWidth = 36;
constexpr int kDriveButtonHeight   = 22;
constexpr int kLayoutMargin        = 4;
constexpr int kLayoutSpacing       = 4;
// Debounce filter-bar typing — shorter feels laggy, longer feels unresponsive.
constexpr int kFilterDebounceMs    = 150;
}

// Filter proxy: passes through ancestors of the current root (so mapFromSource
// stays valid), and applies the wildcard filter to everything else.
class WildcardFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setCurrentPath(const QString &path) { m_currentPath = path; }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto *fs = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (fs && !m_currentPath.isEmpty()) {
            const QString itemPath = QDir::cleanPath(fs->filePath(fs->index(row, 0, parent)));
            const QString rootPath = QDir::cleanPath(m_currentPath);
            // Pass through ancestors of current root so mapFromSource stays valid
            const QString sep = itemPath.endsWith('/') ? "" : "/";
            if (rootPath.compare(itemPath, Qt::CaseInsensitive) == 0 ||
                rootPath.startsWith(itemPath + sep, Qt::CaseInsensitive))
                return true;
        }
        return QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }

private:
    QString m_currentPath;
};

FilePanel::FilePanel(const QString &rootPath, QWidget *parent)
    : QWidget(parent)
{
    setupModel();
    setupUi();
    connectSignals();
    setupKeyboardShortcuts();
    navigateTo(rootPath, false);
}

void FilePanel::setupModel()
{
    m_model = new QFileSystemModel(this);
    m_model->setRootPath("");
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    m_proxyModel = new WildcardFilterProxy(this);
    m_proxyModel->setSourceModel(m_model);
    m_proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxyModel->setFilterKeyColumn(0);
    m_proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void FilePanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(kLayoutMargin, kLayoutMargin, kLayoutMargin, kLayoutMargin);
    mainLayout->setSpacing(kLayoutSpacing);

    auto *addressBarLayout = new QHBoxLayout;

    m_backButton = new QToolButton(this);
    m_backButton->setIcon(QIcon::fromTheme("go-previous"));
    m_backButton->setToolTip("Back (Alt+Left)");
    m_backButton->setFixedSize(kNavButtonSize, kNavButtonSize);
    m_backButton->setEnabled(false);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setIcon(QIcon::fromTheme("go-next"));
    m_forwardButton->setToolTip("Forward (Alt+Right)");
    m_forwardButton->setFixedSize(kNavButtonSize, kNavButtonSize);
    m_forwardButton->setEnabled(false);

    m_addressBar = new QLineEdit(this);
    m_addressBar->setPlaceholderText("Path…");

    addressBarLayout->addWidget(m_backButton);
    addressBarLayout->addWidget(m_forwardButton);
    addressBarLayout->addWidget(m_addressBar);
    mainLayout->addLayout(addressBarLayout);

    auto *driveScroll = new QScrollArea(this);
    driveScroll->setFrameShape(QFrame::NoFrame);
    driveScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    driveScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    driveScroll->setFixedHeight(kDriveBarHeight);

    m_driveBar = new QWidget;
    auto *driveLayout = new QHBoxLayout(m_driveBar);
    driveLayout->setContentsMargins(0, 0, 0, 0);
    driveLayout->setSpacing(2);

    driveScroll->setWidget(m_driveBar);
    driveScroll->setWidgetResizable(true);
    mainLayout->addWidget(driveScroll);

    refreshDriveBar();

    m_listView = new QListView(this);
    m_listView->setModel(m_proxyModel);
    m_listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listView->setViewMode(QListView::ListMode);
    m_listView->setResizeMode(QListView::Adjust);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_listView->setDragDropMode(QAbstractItemView::NoDragDrop);
    // Stylesheet is parsed ONCE here; setActive() only flips the dynamic
    // property below, which avoids re-parsing on every focus change.
    m_listView->setStyleSheet(
        R"(QListView[panelActive="true"] { background-color: rgba(255, 255, 255, 0.08); })");
    m_listView->setProperty("panelActive", false);
    mainLayout->addWidget(m_listView, 1);

    m_filterBar = new QLineEdit(this);
    m_filterBar->setPlaceholderText("Filter… (*.txt, image*, foo?)");
    m_filterBar->setClearButtonEnabled(true);
    mainLayout->addWidget(m_filterBar);

    setFocusProxy(m_listView);
}

void FilePanel::connectSignals()
{
    connect(m_listView, &QListView::activated,
            this,   &FilePanel::onItemActivated);
    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FilePanel::onSelectionChanged);
    connect(m_listView, &QListView::customContextMenuRequested,
            this,   &FilePanel::onContextMenuRequested);
    connect(m_addressBar, &QLineEdit::returnPressed,
            this,         &FilePanel::onAddressBarReturn);
    connect(m_backButton,    &QToolButton::clicked, this, &FilePanel::onGoBack);
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePanel::onGoForward);

    connect(m_model, &QFileSystemModel::directoryLoaded,
            this, [this](const QString &loadedPath) {
                if (QDir::cleanPath(loadedPath) == m_currentPath)
                    updateStatusBar();
            });

    // viewport() must be filtered separately because mouse events (e.g. middle-click)
    // are delivered to the viewport, not to the QListView itself.
    m_listView->installEventFilter(this);
    m_listView->viewport()->installEventFilter(this);
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
    bindShortcut(QKeySequence::SelectAll, [this] { m_listView->selectAll(); });
    bindShortcut(QKeySequence::Back,      [this] { onGoBack(); });
    bindShortcut(QKeySequence::Forward,   [this] { onGoForward(); });

    bindShortcut(Qt::Key_Backspace, [this] {
        QDir dir(m_currentPath);
        if (dir.cdUp())
            navigateTo(dir.absolutePath());
    });

    bindShortcut(Qt::CTRL | Qt::Key_S, [this] {
        m_filterBar->setFocus();
        m_filterBar->selectAll();
    });

    auto *escShortcut = new QShortcut(Qt::Key_Escape, m_filterBar);
    escShortcut->setContext(Qt::WidgetShortcut);

    connect(escShortcut, &QShortcut::activated, this, [this] {
        m_filterBar->clear();
        m_listView->setFocus();
    });
}

QString FilePanel::currentPath() const { return m_currentPath; }

QStringList FilePanel::selectedPaths() const
{
    // QListView only ever exposes column 0, so selectedIndexes() returns one
    // index per row — no column filtering needed.
    QStringList paths;
    for (const QModelIndex &proxyIdx : m_listView->selectionModel()->selectedIndexes()) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        paths << m_model->filePath(srcIdx);
    }
    return paths;
}

void FilePanel::navigateTo(const QString &path, bool addToHistory)
{
    if (addToHistory) {
        const QFileInfo info(path);
        if (!info.exists()) {
            emit statusMessage(QStringLiteral("Path does not exist: %1").arg(path));
            return;
        }
        if (!info.isDir()) {
            emit statusMessage(QStringLiteral("Not a directory: %1").arg(path));
            return;
        }
    }

    if (addToHistory && !m_currentPath.isEmpty() && m_currentPath != QDir::cleanPath(path)) {
        m_backStack.push(m_currentPath);
        m_forwardStack.clear();
    }

    m_currentPath = QDir::cleanPath(path);
    m_proxyModel->setCurrentPath(m_currentPath);

    m_filterDebounceTimer->stop();
    m_filterBar->clear();
    m_proxyModel->setFilterWildcard({});

    const QModelIndex srcRoot = m_model->setRootPath(m_currentPath);
    const QModelIndex proxyRoot = m_proxyModel->mapFromSource(srcRoot);
    m_listView->setRootIndex(proxyRoot);

    updateAddressBar();
    updateStatusBar();
    updateNavButtons();
    emit pathChanged(m_currentPath);
}

QString FilePanel::sanitizeFileName(const QString &name) const
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


bool FilePanel::isValidEntryName(const QString &name, QString &errorString) const
{
    if (name.isEmpty()) {
        errorString = "Name cannot be empty.";
        return false;
    }

    // "." and ".." are reserved on every filesystem (current/parent dir links).
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        errorString = "Names \".\" and \"..\" are reserved.";
        return false;
    }

#ifdef Q_OS_WIN
    // Windows forbidden characters: \ / : * ? " < > |
    static const QRegularExpression forbiddenChars(R"([\\/:*?"<>|])");
    const QRegularExpressionMatch match = forbiddenChars.match(name);
    if (match.hasMatch()) {
        errorString = QString("Names cannot contain '%1'.").arg(match.captured(0));
        return false;
    }

    // Control characters (0x00–0x1F) are not allowed
    for (const QChar &ch : name) {
        if (ch.unicode() < 0x20) {
            errorString = "Names cannot contain control characters.";
            return false;
        }
    }

    // Names must not end with a dot or space (Windows silently strips them)
    if (name.endsWith('.') || name.endsWith(' ')) {
        errorString = "Names must not end with a dot or space.";
        return false;
    }

    // Reserved device names (case-insensitive), with or without extension (e.g. "CON.txt")
    static const QRegularExpression reservedPattern(
        R"(^(CON|PRN|AUX|NUL|COM\d+|LPT\d+)(\..+)?$)",
        QRegularExpression::CaseInsensitiveOption);
    if (reservedPattern.match(name).hasMatch()) {
        errorString = "This name is reserved on Windows.";
        return false;
    }

#else
    // Linux / Unix: only '/' and null byte are forbidden
    if (name.contains('/') || name.contains(QChar(0))) {
        errorString = "Names cannot contain '/' or null characters.";
        return false;
    }
#endif

    errorString.clear();
    return true;
}

void FilePanel::refresh()
{
    if (!QFileInfo(m_currentPath).isDir()) {
        emit statusMessage(tr("Directory no longer exists: %1").arg(m_currentPath));

        QDir dir(m_currentPath);
        while (dir.cdUp()) {
            if (QFileInfo(dir.absolutePath()).isDir()) {
                navigateTo(dir.absolutePath(), false);
                return;
            }
        }
        // Fallback to HomePath
        navigateTo(QDir::homePath(), false);
        return;
    }

    m_model->setRootPath({});
    const QModelIndex srcRoot   = m_model->setRootPath(m_currentPath);
    const QModelIndex proxyRoot = m_proxyModel->mapFromSource(srcRoot);
    m_listView->setRootIndex(proxyRoot);
    updateStatusBar();
}
void FilePanel::createNewFolder()
{
    const QString name = promptValidName(
        "New Folder", "Folder name:", "New Folder",
        /*isDirectory=*/true, m_currentPath);

    if (name.isEmpty())
        return;

    const QString targetPath = QDir(m_currentPath).filePath(name);

    // not using QDir::mkdir() -> returns only bool — std::filesystem gives us the actual error message
    std::error_code ec;
    std::filesystem::create_directory(targetPath.toStdWString(), ec);
    if (ec) {
        QMessageBox::warning(this, "New Folder",
                             QString("Could not create folder \"%1\":\n%2")
                                 .arg(name, QString::fromStdString(ec.message())));
        return;
    }

    refresh();

    // Select and scroll to the new folder so the user can immediately rename or open it.
    const QModelIndex srcIdx = m_model->index(targetPath);
    if (srcIdx.isValid()) {
        const QModelIndex proxyIdx = m_proxyModel->mapFromSource(srcIdx);
        if (proxyIdx.isValid()) {
            m_listView->setCurrentIndex(proxyIdx);
            m_listView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
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

void FilePanel::onItemActivated(const QModelIndex &proxyIndex)
{
    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIndex);
    const QString path = m_model->filePath(srcIdx);
    const QFileInfo info(path);

    if (info.isDir())
        navigateTo(path);
    else
        openWithDefault(path);
}

void FilePanel::onSelectionChanged()
{
    updateStatusBar();
}

void FilePanel::onAddressBarReturn()
{
    navigateTo(m_addressBar->text());
}

void FilePanel::onGoBack()
{
    while (!m_backStack.isEmpty()) {
        const QString prev = m_backStack.pop();
        if (QFileInfo(prev).isDir()) {
            m_forwardStack.push(m_currentPath);
            navigateTo(prev, false);
            return;
        }
    }
    updateNavButtons();
}

void FilePanel::onGoForward()
{
    while (!m_forwardStack.isEmpty()) {
        const QString next = m_forwardStack.pop();
        if (QFileInfo(next).isDir()) {
            m_backStack.push(m_currentPath);
            navigateTo(next, false);
            return;
        }
    }
    updateNavButtons();
}

void FilePanel::onContextMenuRequested(const QPoint &pos)
{
    const QModelIndex proxyIdx = m_listView->indexAt(pos);
    const bool hasSelection = proxyIdx.isValid();

    QMenu menu(this);

    if (hasSelection) {
        const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
        const QString path = m_model->filePath(srcIdx);
        const QFileInfo info(path);

        if (info.isDir()) {
            menu.addAction(QIcon::fromTheme("folder-open"), "Open", [this, path]{ navigateTo(path); });
            menu.addAction(QIcon::fromTheme("tab-new"), "Open in New Tab", [this, path]{ emit openInNewTabRequested(path); });
        }
        else {
            menu.addAction(QIcon::fromTheme("document-open"), "Open",[this, path]{ openWithDefault(path); });
        }

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-copy"), "Copy", [this]{ copyToClipboard(false); });
        menu.addAction(QIcon::fromTheme("edit-cut"),  "Cut", [this]{ copyToClipboard(true); });

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-rename"), "Rename", [this, proxyIdx]{ renameItem(proxyIdx); });

        menu.addAction(QIcon::fromTheme("edit-delete"), "Delete", this, &FilePanel::deleteRequested);
        menu.addSeparator();
    }

    // Paste — always visible, disabled when clipboard has no files
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QAction *pasteAct = menu.addAction(QIcon::fromTheme("edit-paste"), "Paste",
                                       this, &FilePanel::pasteRequested);
    pasteAct->setEnabled(mime && mime->hasUrls());

    menu.addSeparator();
    menu.addAction(QIcon::fromTheme("folder-new"),   "New Folder\tF7", this, &FilePanel::createNewFolder);
    menu.addAction(QIcon::fromTheme("view-refresh"), "Refresh\tF2", this, &FilePanel::refresh);

    menu.exec(m_listView->viewport()->mapToGlobal(pos));
}


void FilePanel::updateAddressBar()
{
    m_addressBar->setText(m_currentPath);
}

void FilePanel::updateStatusBar()
{
    // Use the proxy so the count reflects the active filter, not the raw FS.
    const int total    = m_proxyModel->rowCount(m_listView->rootIndex());
    const int selected = m_listView->selectionModel()->selectedIndexes().count();

    const QString msg = (selected > 0)
        ? QString("%1 items  |  %2 selected  |  %3").arg(total).arg(selected).arg(m_currentPath)
        : QString("%1 items  |  %2").arg(total).arg(m_currentPath);

    emit statusMessage(msg);
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
        "Rename", "New name:", currentName,
        fileInfo.isDir(), fileInfo.absolutePath(), currentPath);

    if (name.isEmpty() || name == currentName)
        return;

    const QString targetPath = fileInfo.dir().filePath(name);

    QFile file(currentPath);
    if (!file.rename(targetPath))
        QMessageBox::warning(this, "Rename failed", file.errorString());
}

void FilePanel::setActive(bool active)
{
    if (m_listView->property("panelActive").toBool() == active)
        return;  // no-op — avoids the polish/unpolish cycle entirely

    m_listView->setProperty("panelActive", active);
    // Re-evaluate the existing (already-parsed) stylesheet against the new
    // property value; cheaper than setStyleSheet() because no QSS re-parse.
    m_listView->style()->unpolish(m_listView);
    m_listView->style()->polish(m_listView);

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

bool FilePanel::handleMiddleClickNavigation(QMouseEvent *mouseEvent)
{
    const QModelIndex proxyIdx = m_listView->indexAt(mouseEvent->pos());
    if (!proxyIdx.isValid())
        return false;

    const QModelIndex srcIdx = m_proxyModel->mapToSource(proxyIdx);
    if (!srcIdx.isValid())
        return false;

    const QString path = m_model->filePath(srcIdx);
    const QFileInfo fileInfo(path);

    if (!fileInfo.isDir())
        return false;

    emit openInNewTabRequested(path);
    return true;
}

namespace {
/// @brief Format byte count as "X MB" or "X GB" for drive-bar tooltips.
QString formatDriveBytes(qint64 bytes){
    if (bytes <= 0) return "?";
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MB").arg(bytes / (1024 * 1024));
    return QString("%1 GB").arg(bytes / (1024LL * 1024 * 1024));
}
}

void FilePanel::refreshDriveBar()
{
    auto *layout = qobject_cast<QHBoxLayout *>(m_driveBar->layout());
    if (!layout)
        return;

    clearLayout(layout);

    for (const QStorageInfo &drive : QStorageInfo::mountedVolumes()) {
        if (!drive.isReady())
            continue;

        if (auto *button = createDriveButton(drive))
            layout->addWidget(button);
    }

    layout->addStretch();
}

void FilePanel::onDriveTopologyChanged()
{
    refreshDriveBar();
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

    // Re-apply root index: proxy mapping changes whenever the filter changes
    const QModelIndex proxyRoot = m_proxyModel->mapFromSource(m_model->index(m_currentPath));
    m_listView->setRootIndex(proxyRoot);

    // Select the first visible item so arrow-key navigation works immediately
    const QModelIndex first = m_proxyModel->index(0, 0, proxyRoot);
    if (first.isValid()) {
        m_listView->setCurrentIndex(first);
        m_listView->scrollTo(first);
    }
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

QString FilePanel::buildDriveLabel(const QStorageInfo &drive) const
{
    const QString rootPath = QDir::toNativeSeparators(drive.rootPath());

    if (rootPath.length() >= 2 && rootPath[1] == ':')
        return rootPath.left(2).toUpper();          // "C:", "D:", …

    return drive.displayName().left(5);
}

QString FilePanel::buildDriveTooltip(const QStorageInfo &drive) const
{
    QString tooltip = QDir::toNativeSeparators(drive.rootPath());

    const QString volumeName    = drive.name();
    const QString fileSystemType = QString::fromUtf8(drive.fileSystemType());

    if (!volumeName.isEmpty())
        tooltip += QString(" (%1)").arg(volumeName);

    if (!fileSystemType.isEmpty())
        tooltip += "\n" + fileSystemType;

    if (drive.bytesTotal() > 0)
        tooltip += QString("\n%1 free of %2")
                       .arg(formatDriveBytes(drive.bytesAvailable()),
                            formatDriveBytes(drive.bytesTotal()));

    return tooltip;
}

QToolButton *FilePanel::createDriveButton(const QStorageInfo &drive)
{
    const QString label = buildDriveLabel(drive);
    if (label.isEmpty())
        return nullptr;

    auto *button = new QToolButton(m_driveBar);
    button->setText(label);
    button->setFixedHeight(kDriveButtonHeight);
    button->setMinimumWidth(kDriveButtonMinWidth);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setToolTip(buildDriveTooltip(drive));

    const QString targetPath = drive.rootPath();
    connect(button, &QToolButton::clicked, this, [this, targetPath] {
        navigateTo(targetPath);
    });

    return button;
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
                                     isDirectory ? "A folder with this name already exists."
                                                 : "A file or folder with this name already exists.");
                lastSuggestion = name;
                continue;
            }
        }

        return name;
    }
}

void FilePanel::clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
            delete item;                // delete the QWidgetItem wrapper only
        } else if (QLayout *childLayout = item->layout()) {
            clearLayout(childLayout);
            // QLayoutItem == QLayout here — deleteLater handles the deletion;
            // a synchronous `delete item` would race with pending events.
            childLayout->deleteLater();
        } else {
            delete item;                // QSpacerItem etc.
        }
    }
}

bool FilePanel::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_filterBar && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down ||
            key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
            QApplication::sendEvent(m_listView, event);
            return true;
        }
    }

    if (obj == m_listView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        if (mouseEvent->button() == Qt::MiddleButton) {
            if (handleMiddleClickNavigation(mouseEvent))
                return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
