#pragma once

#include <QHash>
#include <QObject>
#include <QPromise>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <optional>

/// Size and content counts of one folder (everything below it, links not followed).
struct FolderStats
{
    qint64 bytes   = 0;
    qint64 files   = 0;
    qint64 folders = 0;
};

/// One folder of a finished size calculation.
struct FolderSize
{
    QString     path;
    FolderStats stats;
};

/**
 * @brief Folder sizes calculated this session, shared by every FilePanel.
 *
 * Lives in memory only — never saved. Folders change while the app is closed,
 * so every start begins empty and sizes are calculated again on request.
 *
 * One walk records every subfolder it passes, so after calculating a folder
 * its subfolders show their sizes immediately, in every tab.
 *
 * Keys are case-insensitive on Windows (NTFS / SMB), so two panels that
 * reached the same folder with different spelling share the entry.
 */
class FolderSizeCache : public QObject
{
    Q_OBJECT

public:
    explicit FolderSizeCache(QObject *parent = nullptr);

    bool contains(const QString &path) const;
    std::optional<FolderStats> stats(const QString &path) const;

    /// @brief Store the results of one walk. Emits changed().
    void insert(const QVector<FolderSize> &sizes);
    /// @brief Forget one folder (it is about to be recalculated). Emits changed().
    void remove(const QString &path);
    /// @brief Forget @p path, everything below it and all its ancestors — their
    /// totals include it. Called when its contents may have changed. Emits changed().
    void invalidate(const QString &path);

    /// @brief Pool-thread entry point for QtConcurrent::run.
    ///
    /// One result per entry of @p folders: all its subfolders (post-order) followed
    /// by the folder itself as the last element. @p bytesSoFar counts live for the
    /// progress display. Blocking — can take minutes on a network drive.
    static void calculate(QPromise<QVector<FolderSize>> &promise, const QStringList &folders,
                          std::shared_ptr<std::atomic<qint64>> bytesSoFar);

signals:
    void changed();

private:
    static QString key(const QString &path);

    QHash<QString, FolderStats> m_stats;
};
