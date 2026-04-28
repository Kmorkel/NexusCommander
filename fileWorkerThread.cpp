
#include <QDir>
#include <filesystem>
#include "fileWorkerThread.h"

static constexpr qint64 kChunkSizeMax            = 1024 * 1024;   // 1 MiB
static constexpr qint64 kChunkSizeMin            = 64 * 1024;     // 64 KiB

// Re-evaluate adaptiveChunkSize() inside copyFile() at this cadence so a
// mid-copy speed-limit change takes effect within the same file.
static constexpr int    kChunkReevalIntervalMs    = 250;
// Throttle jobProgress emissions — copyFile produces ~1 chunk/ms on a fast SSD,
// emitting that often would saturate the UI thread queue.
static constexpr int    kProgressSignalIntervalMs = 50;
// Upper bound for makeUniquePath()'s "file(N)" search before giving up.
static constexpr int    kMaxUniqueSuffix          = 100000;
// Speed-limit cycle target — chunk sized so we write this many bursts per second.
static constexpr int    kSpeedLimitUpdatesPerSec  = 10;

namespace {

/// RAII guard for an in-progress file write, with optional backup/rollback
/// of an existing original file.
///
/// On the failure path (destructor without prior commit()):
///   1. remove the partial file at @c targetPath
///   2. if @c backupPath is set, rename it back to @c targetPath
/// On the success path (commit() was called):
///   - drop the backup, if any — the new file has replaced it.
///
/// The surrounding QFile closes itself on stack unwind, so the guard does
/// not need to hold the handle.
struct FileReplaceGuard {
    QString targetPath;
    QString backupPath;
    bool    committed = false;

    explicit FileReplaceGuard(const QString& target = {},
                              const QString& backup = {})
        : targetPath{target}
        , backupPath{backup}
    {}

    ~FileReplaceGuard() {
        if (committed) {
            dropBackup();
        } else {
            restoreBackup();
        }
    }

    FileReplaceGuard(const FileReplaceGuard&)            = delete;
    FileReplaceGuard& operator=(const FileReplaceGuard&) = delete;
    FileReplaceGuard(FileReplaceGuard&&)                 = delete;
    FileReplaceGuard& operator=(FileReplaceGuard&&)      = delete;

    void setTarget(const QString& p) { targetPath = p; }
    void setBackup(const QString& p) { backupPath = p; }
    void commit()                    { committed = true; }

private:
    // Success: the new file has replaced the original; drop the backup.
    void dropBackup() {
        if (!backupPath.isEmpty() && !QFile::remove(backupPath)) {
            qWarning() << "Failed to remove backup file:" << backupPath;
        }
    }

    // Failure: delete the partial target, then restore the backup if present.
    void restoreBackup() {
        if (!targetPath.isEmpty()) {
            QFile::remove(targetPath); // best-effort; may not exist
        }
        if (!backupPath.isEmpty() && !targetPath.isEmpty()
            && !QFile::rename(backupPath, targetPath)) {
            qWarning() << "Rollback failed: original kept at" << backupPath;
        }
    }
};

// Unique backup path next to the target: "<dst>.ncbak-<8 hex chars>".
QString makeBackupPath(const QString &dst) {
    return dst + QStringLiteral(".ncbak-")
    + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

// Consistent "<verb> \"<file>\": <reason>" error messages.
QString fileError(const QString &verb, const QString &path, const QString &reason) {
    return QStringLiteral("%1 \"%2\": %3").arg(verb, QFileInfo(path).fileName(), reason);
}

// If @p dst exists, rename it to a backup path and register that path on
// @p guard so it is restored on failure. Returns false and fills
// @p job.errorMessage when the rename fails.
bool prepareDestination(const QString &dst, FileJob &job, FileReplaceGuard &guard) {
    const QFileInfo dstInfo(dst);
    if (!dstInfo.exists() && !dstInfo.isSymLink())
        return true;

    // dst exists — opening WriteOnly would truncate it immediately, so any
    // mid-copy error would leave the user with an empty/partial file.
    // Move it aside first; the guard restores it if we fail.
    const QString backupPath = makeBackupPath(dst);
    if (!QDir().rename(dst, backupPath)) {
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = fileError(QStringLiteral("Cannot replace"), dst,
                                         QStringLiteral("rename failed"));
        }
        return false;
    }
    guard.setBackup(backupPath);
    return true;
}

}

FileWorkerThread::FileWorkerThread(QObject *parent)
    : QThread(parent)
{
    start();  // Thread starts immediately, and blocks on m_condition until a job arrives
}

FileWorkerThread::~FileWorkerThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stop = true;
        if (!m_runningJobId.isNull())
            m_cancelled.insert(m_runningJobId);
        for (const auto &job : std::as_const(m_queue))
            m_cancelled.insert(job.id);
        m_condition.wakeAll();
        m_pauseCondition.wakeAll();
    }

    {
        // Unblock the worker if it is parked on the conflict dialog handshake.
        QMutexLocker lock(&m_conflictMutex);
        m_conflictPending = false;
        m_conflictCondition.wakeAll();
    }
    wait();
}

void FileWorkerThread::enqueue(const FileJob &job)
{
    QMutexLocker lock(&m_mutex);
    m_queue.enqueue(job);
    m_condition.wakeOne();
}

void FileWorkerThread::cancelJob(const QUuid &id)
{
    QMutexLocker lock(&m_mutex);
    m_cancelled.insert(id);
    m_paused.remove(id);
    m_pauseCondition.wakeAll();
}

void FileWorkerThread::cancelAll()
{
    QMutexLocker lock(&m_mutex);
    // Currently running job is already dequeued, so it must be added explicitly.
    if (!m_runningJobId.isNull())
        m_cancelled.insert(m_runningJobId);
    for (const auto &job : std::as_const(m_queue))
        m_cancelled.insert(job.id);
    m_paused.clear();

    // Reset the global pause flag — otherwise every newly enqueued job after
    // this point would silently start paused.
    m_globalPause = false;

    m_pauseCondition.wakeAll();
}

void FileWorkerThread::pauseJob(const QUuid &id)
{
    QMutexLocker lock(&m_mutex);
    m_paused.insert(id);
    // Break any in-flight sleepInterruptible() immediately — otherwise the
    // pause would only take effect after the current sleep slice expires.
    m_pauseCondition.wakeAll();
}

void FileWorkerThread::resumeJob(const QUuid &id)
{
    QMutexLocker lock(&m_mutex);
    m_paused.remove(id);
    m_pauseCondition.wakeAll();
}

void FileWorkerThread::pauseAll()
{
    QMutexLocker lock(&m_mutex);
    m_globalPause = true;
    if (!m_runningJobId.isNull())
        m_paused.insert(m_runningJobId);
    for (const auto &job : std::as_const(m_queue))
        m_paused.insert(job.id);
    m_pauseCondition.wakeAll();
}

void FileWorkerThread::resumeAll()
{
    QMutexLocker lock(&m_mutex);
    m_globalPause = false;
    m_paused.clear();
    m_pauseCondition.wakeAll();
}

bool FileWorkerThread::isGlobalPaused() const
{
    QMutexLocker lock(&m_mutex);
    return m_globalPause;
}

void FileWorkerThread::setSpeedLimitMBs(double mbps)
{
    QMutexLocker lock(&m_mutex);
    // UI input is MiB/s; internal state is bytes/s so the throttling math
    // never has to touch kBytesPerMiB again.
    m_speedLimitBps = mbps * kBytesPerMiB;
}


QStringList FileWorkerThread::activeSourcePaths() const
{
    QMutexLocker lock(&m_mutex);
    QStringList paths = m_runningJobSources;
    for (const FileJob &job : m_queue)
        paths << job.sources;
    return paths;
}

void FileWorkerThread::resolveConflict(FileJob::ConflictAction action, bool applyToAll)
{
    QMutexLocker lock(&m_conflictMutex);
    m_conflictResult     = action;
    m_conflictApplyToAll = applyToAll;
    m_conflictPending    = false;
    m_conflictCondition.wakeOne();
}


void FileWorkerThread::run()
{
    while (true) {
        FileJob job;

        {
            QMutexLocker lock(&m_mutex);

            while (m_queue.isEmpty() && !m_stop)
                m_condition.wait(&m_mutex);

            if (m_stop)
                return;

            job = m_queue.dequeue();
            m_runningJobId      = job.id;
            m_runningJobSources = job.sources;
            if (m_globalPause)
                m_paused.insert(job.id);
        }

        if (isCancelled(job.id)) {
            {
                QMutexLocker lock(&m_mutex);
                m_cancelled.remove(job.id);
                m_paused.remove(job.id);
                // Mirror the cleanup of the normal path — otherwise
                // activeSourcePaths() would report this cancelled job
                // as "in use" until the next dequeue.
                m_runningJobId = QUuid();
                m_runningJobSources.clear();
            }
            emit jobFinished(job.id, FileJob::Status::Cancelled, {});
            continue;
        }

        processJob(job);

        bool done = false;
        {
            QMutexLocker lock(&m_mutex);
            // m_runningJobId / m_runningJobSources already cleared in finalizeJob.
            done = m_queue.isEmpty();
        }

        // Emit outside mutex to prevent potential deadlock in connected slots.
        if (done)
            emit allJobsDone();
    }
}


void FileWorkerThread::processJob(FileJob &job)
{
    job.status = FileJob::Status::Running;

    // calculateTotalBytes is pausable/cancellable so the user is not stuck on
    // a multi-million-file scan before "Pause" or "Cancel" can take effect.
    switch (job.type) {
    case FileJob::Type::Copy:
        emit jobScanning(job.id);
        job.bytesTotal = calculateTotalBytes(job.sources, job.id);
        break;
    case FileJob::Type::Move:
        emit jobScanning(job.id);
        // Move records per-source sizes so moveSource() can bump progress on a
        // same-volume rename without re-walking the source tree.
        job.bytesTotal = calculateTotalBytes(job.sources, job.id, &job.bytesPerSource);
        break;
    case FileJob::Type::Delete:
        job.bytesTotal = job.sources.size();
        break;
    }

    emit jobStarted(job.id, job.bytesTotal);

    switch (job.type) {
    case FileJob::Type::Copy:   runCopyJob(job);   break;
    case FileJob::Type::Move:   runMoveJob(job);   break;
    case FileJob::Type::Delete: runDeleteJob(job); break;
    }
}

// Only called once per job, after all work has completed — never mid-operation.
// Partial-file cleanup is handled by PartialFileGuard inside copyFile().
void FileWorkerThread::finalizeJob(FileJob &job)
{
    if (job.symlinksSkipped > 0) {
        const QString warning = QString("%1 symlink(s) skipped")
                                    .arg(job.symlinksSkipped);
        if (job.warningMessage.isEmpty())
            job.warningMessage = warning;
        else
            job.warningMessage += "; " + warning;
    }

    if (isCancelled(job.id))
        job.status = FileJob::Status::Cancelled;
    else if (!job.errorMessage.isEmpty())
        job.status = FileJob::Status::Failed;
    else if (!job.warningMessage.isEmpty())
        job.status = FileJob::Status::DoneWithWarnings;
    else
        job.status = FileJob::Status::Done;

    // UI receives warnings via errorMessage slot for now — prefix tells them apart.
    QString finalMessage = job.errorMessage;
    if (!job.warningMessage.isEmpty()) {
        if (!finalMessage.isEmpty())
            finalMessage += "; ";
        finalMessage += job.warningMessage;
    }

    emit jobFinished(job.id, job.status, finalMessage);

    QMutexLocker lock(&m_mutex);
    m_cancelled.remove(job.id);
    m_paused.remove(job.id);
    m_runningJobId      = QUuid();
    m_runningJobSources.clear();
}

void FileWorkerThread::runCopyJob(FileJob &job)
{
    qint64 bytesDone = 0;
    FileJob::ConflictAction conflictAction = FileJob::ConflictAction::Ask;
    int copiedCount = 0;
    int failedCount = 0;
    QStringList failedNames;

    for (const QString &sourcePath : std::as_const(job.sources)) {
        if (isCancelled(job.id)) break;
        waitIfPaused(job.id);
        if (isCancelled(job.id)) break;

        const QFileInfo sourceInfo(sourcePath);

        // Symlinks are skipped consistently at every recursion level —
        // calculateTotalBytes also ignores them, so the progress math stays correct.
        if (sourceInfo.isSymLink()) {
            ++job.symlinksSkipped;
            continue;
        }

        const QString destPath = resolveDestinationConflict(
            QDir(job.destination).filePath(sourceInfo.fileName()),
            job.id,
            conflictAction);

        if (destPath.isEmpty())
            continue;

        bool success;
        if (sourceInfo.isDir())
            success = copyRecursive(sourcePath, destPath, job, bytesDone, conflictAction);
        else
            success = copyFile(sourcePath, destPath, job, bytesDone);

        if (success) {
            ++copiedCount;
        } else if (!isCancelled(job.id)) {
            ++failedCount;
            failedNames << sourceInfo.fileName();

            if (job.errorMessage.isEmpty())
                job.errorMessage = QStringLiteral("Copy failed: \"%1\"").arg(sourceInfo.fileName());
        }
    }

    if (failedCount > 0) {
        const QString summary = (copiedCount > 0)
            ? QStringLiteral("%1 copied, %2 failed (%3)")
                  .arg(copiedCount).arg(failedCount).arg(failedNames.join(", "))
            : QStringLiteral("All %1 item(s) failed (%2)")
                  .arg(failedCount).arg(failedNames.join(", "));

        // Preserve the specific reason (set by the first failing copyFile call)
        // by appending it after the summary instead of overwriting it.
        job.errorMessage = job.errorMessage.isEmpty()
                               ? summary
                               : summary + QStringLiteral(" — ") + job.errorMessage;
    }

    finalizeJob(job);
}

void FileWorkerThread::runMoveJob(FileJob &job)
{
    qint64 bytesDone = 0;
    FileJob::ConflictAction conflictAction = FileJob::ConflictAction::Ask;
    int movedCount  = 0;
    int failedCount = 0;
    QStringList failedNames;

    for (const QString &src : std::as_const(job.sources)) {
        if (isCancelled(job.id)) break;
        waitIfPaused(job.id);
        if (isCancelled(job.id)) break;

        const QFileInfo sourceInfo(src);

        if (sourceInfo.isSymLink()) {
            ++job.symlinksSkipped;
            continue;
        }

        const QString rawDst = QDir(job.destination).filePath(sourceInfo.fileName());
        const QString dst    = resolveDestinationConflict(rawDst, job.id, conflictAction);
        if (dst.isEmpty()) continue;

        if (moveSource(src, dst, sourceInfo, job, bytesDone, conflictAction)) {
            ++movedCount;
        } else if (!isCancelled(job.id)) {
            ++failedCount;
            failedNames << sourceInfo.fileName();

            if (job.errorMessage.isEmpty())
                job.errorMessage = QStringLiteral("Move failed: \"%1\"").arg(sourceInfo.fileName());
        }
    }

    if (failedCount > 0) {
        const QString summary = (movedCount > 0)
            ? QStringLiteral("%1 moved, %2 failed (%3)")
                  .arg(movedCount).arg(failedCount).arg(failedNames.join(", "))
            : QStringLiteral("All %1 item(s) failed (%2)")
                  .arg(failedCount).arg(failedNames.join(", "));

        job.errorMessage = job.errorMessage.isEmpty()
                               ? summary
                               : summary + QStringLiteral(" — ") + job.errorMessage;
    }

    finalizeJob(job);
}

void FileWorkerThread::runDeleteJob(FileJob &job)
{
    int deletedCount  = 0;
    int failedCount   = 0;
    qint64 processed  = 0;
    QStringList failedNames;

    for (const QString &path : std::as_const(job.sources)) {
        if (isCancelled(job.id)) break;
        waitIfPaused(job.id);
        if (isCancelled(job.id)) break;

        const QFileInfo entry(path);

        // Symlinks and regular files use the same removal call;
        // directories need a cancellable recursive delete.
        bool deleted = false;
        if (entry.isDir() && !entry.isSymLink())
            deleted = removeRecursive(path, job);
        else
            deleted = QFile::remove(path);

        if (!deleted && !isCancelled(job.id)) {
            ++failedCount;
            failedNames << entry.fileName();
            if (job.errorMessage.isEmpty())
                job.errorMessage = QStringLiteral("Delete failed: \"%1\"").arg(entry.fileName());
        } else if (deleted) {
            ++deletedCount;
        }

        ++processed;
        emit jobProgress(job.id, processed);
    }

    if (failedCount > 0) {
        const QString summary = (deletedCount > 0)
            ? QStringLiteral("%1 deleted, %2 failed (%3)")
                  .arg(deletedCount).arg(failedCount).arg(failedNames.join(", "))
            : QStringLiteral("All %1 item(s) failed (%2)")
                  .arg(failedCount).arg(failedNames.join(", "));
        job.errorMessage = job.errorMessage.isEmpty()
                               ? summary
                               : summary + QStringLiteral(" — ") + job.errorMessage;
    }

    finalizeJob(job);
}

bool FileWorkerThread::removeRecursive(const QString &path, const FileJob &job)
{
    const QFileInfo info(path);

    if (!info.exists() && !info.isSymLink())
        return true;

    // Symlinks to directories: delete the link itself, not the target.
    if (info.isSymLink() || !info.isDir())
        return QFile::remove(path);

    constexpr auto filter = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                            | QDir::Hidden | QDir::System;

    for (const QFileInfo &entry : QDir(path).entryInfoList(filter)) {
        // isCancelled() covers stop too — the destructor cancels the running job.
        if (isCancelled(job.id))
            return false;

        const QString child = entry.absoluteFilePath();
        bool ok = false;
        if (entry.isDir() && !entry.isSymLink())
            ok = removeRecursive(child, job);
        else
            ok = QFile::remove(child);

        if (!ok)
            return false;
    }

    return QDir().rmdir(path);
}

bool FileWorkerThread::moveSource(const QString &src, const QString &dst, const QFileInfo &srcInfo, FileJob &job,
                                      qint64 &bytesDone, FileJob::ConflictAction &conflictAction)
{
    // Looked up from the pre-scan recorded by calculateTotalBytes — avoids a
    // second recursive walk just to know how far to bump progress on rename.
    const qint64 srcBytes = job.bytesPerSource.value(src, 0);

    // resolveDestinationConflict handed us a dst that the user already agreed to overwrite
    // (otherwise it would have returned an empty string or a unique name).
    // Stash the existing target under a temp name first — if the rename or the
    // cross-device copy then fails, we can roll the original back in. Deleting
    // dst up front would lose data on any failure between delete and rename.
    QString stashPath;
    const QFileInfo dstInfo(dst);
    if (dstInfo.exists() || dstInfo.isSymLink()) {
        stashPath = makeBackupPath(dst);
        if (!QDir().rename(dst, stashPath)) {
            job.errorMessage = QStringLiteral("Move failed: could not replace \"%1\"")
                                   .arg(QFileInfo(dst).fileName());
            return false;
        }
    }

    // Skips the cancel check on purpose — once invoked, cleanup must finish.
    const auto removeFileOrDir = [](const QString &path) {
        const QFileInfo fileInfo(path);
        if (!fileInfo.exists() && !fileInfo.isSymLink()) return;

        if (fileInfo.isDir() && !fileInfo.isSymLink())
            QDir(path).removeRecursively();
        else
            QFile::remove(path);
    };

    const auto discardStash = [&] {
        if (!stashPath.isEmpty()) removeFileOrDir(stashPath);
    };

    const auto rollbackStash = [&] {
        if (stashPath.isEmpty()) return;
        removeFileOrDir(dst);                            // remove any partial copy
        if (!QDir().rename(stashPath, dst)) { // best effort
            qWarning("Move rollback failed: original stashed at \"%s\"",
                     qUtf8Printable(stashPath));
        }
    };

    const bool renamed = srcInfo.isDir()
                             ? QDir().rename(src, dst)
                             : QFile::rename(src, dst);

    if (renamed) {
        discardStash();
        // No bytes were actually written — the OS just relinked the inode.
        // Still bump the counter so the progress bar advances.
        bytesDone += srcBytes;
        emit jobProgress(job.id, bytesDone);
        return true;
    }

    if (isCancelled(job.id)) {
        rollbackStash();
        return false;
    }

    // Slow path: cross-device / cross-partition move (copy + delete).
    // dst is currently free — the original is parked at stashPath — so
    // moveByCopy can write straight in without re-triggering conflict logic.
    if (moveByCopy(src, dst, job, bytesDone, conflictAction)) {
        discardStash();
        return true;
    }

    rollbackStash();
    return false;
}

/// @brief Return a chunk size adapted to the current speed limit.
///
///  - Uncapped:    1 MB  — maximum throughput, minimal syscall overhead
///  - Limited:     chunk sized so we write ~10 bursts per second (100 ms cycle)
///                 → each burst is short, disk I/O looks smoother in Task Manager
///  - Minimum:     64 KB — below this, syscall overhead hurts performance
qint64 FileWorkerThread::adaptiveChunkSize() const
{
    QMutexLocker lock(&m_mutex);

    if (m_speedLimitBps <= 0.0)
        return kChunkSizeMax;

    // Bytes per update = (limit in bytes/s) / desired updates per second
    const qint64 bytesPerUpdate = qint64(m_speedLimitBps) / kSpeedLimitUpdatesPerSec;

    return qBound(kChunkSizeMin, bytesPerUpdate, kChunkSizeMax);
}

bool FileWorkerThread::copyFile(const QString &src, const QString &dst,
                           FileJob &job, qint64 &bytesDone)
{
    QFile sourceFile(src);

    // Constructed BEFORE destFile so the destruction order is:
    //   1. destFile.close()  — closes the handle first
    //   2. guard.~()         — THEN removes the partial file / restores backup
    //   3. sourceFile.close()
    // On Windows, QFile::remove/rename fails with ERROR_SHARING_VIOLATION while
    // the destination file is still open — Qt opens without FILE_SHARE_DELETE.
    FileReplaceGuard guard{dst};
    QFile destFile(dst);

    const auto setErrorOnce = [&](const QString &msg) {
        if (job.errorMessage.isEmpty())
            job.errorMessage = msg;
    };

    if (!prepareDestination(dst, job, guard))
        return false;

    if (!sourceFile.open(QIODevice::ReadOnly)) {
        setErrorOnce(fileError(QStringLiteral("Cannot read"), src, sourceFile.errorString()));
        return false;
    }
    if (!destFile.open(QIODevice::WriteOnly)) {
        setErrorOnce(fileError(QStringLiteral("Cannot write"), dst, destFile.errorString()));
        return false;
    }

    SpeedTracker speed;
    speed.start();

    QElapsedTimer signalTimer;
    signalTimer.start();

    // Re-evaluate chunk size periodically so a mid-copy speed-limit change
    // takes effect inside the same file. Once per ~250 ms is plenty.
    qint64 chunkSize = adaptiveChunkSize();
    QElapsedTimer chunkSizeTimer;
    chunkSizeTimer.start();

    while (!sourceFile.atEnd()) {
        waitIfPaused(job.id);
        if (isCancelled(job.id))
            return false;

        if (chunkSizeTimer.elapsed() >= kChunkReevalIntervalMs) {
            chunkSize = adaptiveChunkSize();
            chunkSizeTimer.restart();
        }

        const QByteArray chunk = sourceFile.read(chunkSize);
        if (chunk.isEmpty()) {
            if (sourceFile.error() != QFileDevice::NoError) {
                setErrorOnce(fileError(QStringLiteral("Read error on"), src,
                                       sourceFile.errorString()));
                return false;
            }
            break;
        }

        if (destFile.write(chunk) != chunk.size()) {
            setErrorOnce(fileError(QStringLiteral("Write error on"), dst,
                                   destFile.errorString()));
            return false;
        }

        bytesDone += chunk.size();
        speed.addBytes(chunk.size());
        applySpeedThrottle(speed);

        if (signalTimer.elapsed() >= kProgressSignalIntervalMs || sourceFile.atEnd()) {
            emit jobProgress(job.id, bytesDone);
            signalTimer.restart();
        }

        speed.resetIfWindowExpired();
    }

    // Best-effort: close explicitly before touching metadata so Windows lets us
    // update the handle. Failures are ignored — the copy itself succeeded.
    destFile.flush();
    destFile.close();

    // Commit before metadata: once the content is on disk the copy is "done".
    // Any later step (mtime, permissions) is best-effort and must not be able
    // to trigger the guard destructor and wipe a valid file.
    guard.commit();

    copyMetadata(src, dst);
    return true;
}

void FileWorkerThread::copyMetadata(const QString &src, const QString &dst) const
{
    const auto perms = QFile(src).permissions();
    if (perms != 0)
        QFile(dst).setPermissions(perms);

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(src.toStdWString(), ec);
    if (!ec)
        std::filesystem::last_write_time(dst.toStdWString(), mtime, ec);
}

bool FileWorkerThread::copyRecursive(const QString &src, const QString &dst,
                                FileJob &job, qint64 &bytesDone, FileJob::ConflictAction &conflictAction)
{
    if (!QDir(dst).mkpath(".")) {
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = QStringLiteral("Copy failed: \"%1\": cannot create directory")
                                   .arg(QDir::toNativeSeparators(dst));
        }
        return false;
    }

    constexpr auto filter = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                            | QDir::Hidden | QDir::System;
    bool allOk = true;

    for (const QFileInfo &entry : QDir(src).entryInfoList(filter)) {
        if (entry.isSymLink()) {
            ++job.symlinksSkipped;
            continue;
        }

        if (isCancelled(job.id))
            return false;

        waitIfPaused(job.id);

        if (isCancelled(job.id))
            return false;

        const QString srcPath = entry.absoluteFilePath();
        const QString dstPath = QDir(dst).filePath(entry.fileName());

        if (entry.isDir()) {
            if (!copyRecursive(srcPath, dstPath, job, bytesDone, conflictAction)) {
                // Cancel propagates up immediately; hard errors only flip the flag
                // so the remaining siblings still get copied.
                if (isCancelled(job.id))
                    return false;
                allOk = false;
            }
            continue;
        }

        const QString resolvedPath = resolveDestinationConflict(dstPath, job.id, conflictAction);
        if (resolvedPath.isEmpty())
            continue;   // user chose Skip

        if (!copyFile(srcPath, resolvedPath, job, bytesDone)) {
            if (isCancelled(job.id))
                return false;
            allOk = false;
        }
    }

    // Apply mtime/permissions AFTER the children — every file written into the
    // directory bumps its mtime, so this has to come last.
    copyMetadata(src, dst);
    return allOk;
}

bool FileWorkerThread::moveByCopy(const QString &src, const QString &dst, FileJob &job, qint64 &bytesDone,
                                  FileJob::ConflictAction &conflictAction)
{
    const QFileInfo sourceInfo(src);

    // 1. Copy source
    bool copied = false;
    if (sourceInfo.isDir())
        copied = copyRecursive(src, dst, job, bytesDone, conflictAction);
    else
        copied = copyFile(src, dst, job, bytesDone);

    if (!copied)
        return false;

    // 2. Delete source — use the cancellable walker so the user can still abort.
    bool deleted = false;
    if (sourceInfo.isDir())
        deleted = removeRecursive(src, job);
    else
        deleted = QFile::remove(src);

    if (!deleted) {
        // Cancel path: finalizeJob will mark the job Cancelled — leaving
        // errorMessage empty prevents a bogus "delete after copy" tooltip.
        if (isCancelled(job.id))
            return false;
        job.errorMessage = QStringLiteral("Move failed: \"%1\": source could not be deleted after copy")
        .arg(sourceInfo.fileName());
        return false;
    }

    return true;
}

// ── Conflict resolution ───────────────────────────────────────────────────────
//
// Returns the final destination path, or an empty string to skip the file.
//   - No conflict       → dst unchanged
//   - Per-job default   → applied directly (no prompt)
//   - defaultAction=Ask → prompts the UI and blocks until resolveConflict()
// "Apply to all" propagates the choice back via defaultAction.
//
QString FileWorkerThread::resolveDestinationConflict(const QString &dst, const QUuid &jobId,
                                          FileJob::ConflictAction &defaultAction)
{
    if (!QFile::exists(dst) && !QFileInfo(dst).isSymLink())
        return dst;

    const auto applyAction = [&](FileJob::ConflictAction action) -> QString {
        switch (action) {
        case FileJob::ConflictAction::Overwrite: return dst;
        case FileJob::ConflictAction::Skip:      return {};
        case FileJob::ConflictAction::KeepBoth:  return makeUniquePath(dst);
        case FileJob::ConflictAction::Ask:       return {};  // unreachable here
        }
        return {};
    };

    // Fast path: a previous "Apply to all" already decided for this job.
    if (defaultAction != FileJob::ConflictAction::Ask)
        return applyAction(defaultAction);

    // Slow path: prompt the UI and block until resolveConflict() wakes up the worker thread.
    FileJob::ConflictAction resolved;
    bool applyToAll;
    {
        QMutexLocker lock(&m_conflictMutex);
        m_conflictPending = true;

        lock.unlock();
        emit conflictDetected(jobId, QFileInfo(dst).fileName(), dst);
        lock.relock();

        while (m_conflictPending && !m_stop)
            m_conflictCondition.wait(&m_conflictMutex);

        if (m_stop) return {};  // shutting down — let run() exit cleanly

        // Copy under lock so the reads below are properly synchronized
        // with the UI thread's writes in resolveConflict().
        resolved   = m_conflictResult;
        applyToAll = m_conflictApplyToAll;
    }

    if (applyToAll)
        defaultAction = resolved;

    return applyAction(resolved);
}

// Generates a unique path: file.txt → file(2).txt → file(3).txt …
QString FileWorkerThread::makeUniquePath(const QString &dst) const
{
    const QFileInfo info(dst);
    const QString baseName  = info.completeBaseName();
    const QString extension = info.suffix().isEmpty() ? QString() : "." + info.suffix();
    const QString parentDir = info.absolutePath();

    for (int counter = 2; counter < kMaxUniqueSuffix; ++counter) {
        const QString candidate = QString("%1/%2(%3)%4")
        .arg(parentDir, baseName)
            .arg(counter)
            .arg(extension);

        if (!QFile::exists(candidate))
            return candidate;
    }

    // Fallback: returning dst would overwrite the existing file silently —
    // exactly what "Keep Both" promised NOT to do. Return empty so the caller
    // treats this like Skip and the user keeps their data.
    return {};
}

// ── Pause logic ───────────────────────────────────────────────────────────────
//
// Called inside the chunk loop. Blocks the worker thread as long as the job
// is paused, then emits jobResumed once it continues.
//
void FileWorkerThread::waitIfPaused(const QUuid &id)
{
    QMutexLocker lock(&m_mutex);

    if (!m_paused.contains(id)) return;

    lock.unlock();
    emit jobPaused(id);
    lock.relock();

    // Wait until resumeJob() or cancelJob() is called.
    while (m_paused.contains(id) && !m_cancelled.contains(id) && !m_stop)
        m_pauseCondition.wait(&m_mutex);

    const bool shouldEmitResumed = !m_cancelled.contains(id) && !m_stop;
    lock.unlock();

    if (shouldEmitResumed)
        emit jobResumed(id);
}

void FileWorkerThread::applySpeedThrottle(SpeedTracker &tracker)
{
    double limitBps;
    { QMutexLocker lock(&m_mutex); limitBps = m_speedLimitBps; }

    if (limitBps <= 0.0)
        return;

    // How long should the current bytes have taken at the target rate?
    // Both sides are already in bytes — no unit conversion needed.
    const double targetMs = tracker.bytesThisSecond / limitBps * 1000.0;
    const qint64 sleepMs  = qint64(targetMs) - tracker.timer.elapsed();

    if (sleepMs <= 0)
        return;

    // Sleep in small slices so cancel/pause remain responsive
    sleepInterruptible(sleepMs);
}

bool FileWorkerThread::shouldAbortSleep() const
{
    // m_mutex must be held by the caller
    return m_stop
        || m_cancelled.contains(m_runningJobId)
        || m_paused.contains(m_runningJobId);
}

void FileWorkerThread::sleepInterruptible(qint64 totalMs)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer elapsed;
    elapsed.start();

    while (!shouldAbortSleep()) {
        const qint64 remaining = totalMs - elapsed.elapsed();
        if (remaining <= 0)
            break;

        // Blocks until woken (cancel/pause/stop) or timeout — no polling.
        // Note: the OS may overshoot by a few ms per wait (thread scheduling
        // granularity), which causes the displayed speed to read ~1-3% below
        // the configured limit. This is expected and not correctable at
        // application level — the displayed value is the true transfer rate.
        m_pauseCondition.wait(&m_mutex, static_cast<unsigned long>(remaining));
    }
}

qint64 FileWorkerThread::calculateTotalBytes(const QStringList &paths, const QUuid &jobId,
                                             QHash<QString, qint64> *perSourceBytes)
{
    qint64 total = 0;
    for (const QString &path : std::as_const(paths)) {
        if (isCancelled(jobId)) break;
        waitIfPaused(jobId);
        if (isCancelled(jobId)) break;

        const QFileInfo fileInfo(path);
        // Symlinks are skipped everywhere in copy/move (see runCopyJob / copyRecursive),
        // so they must not contribute to bytesTotal either — otherwise the progress
        // bar would never reach 100%.
        if (fileInfo.isSymLink())
            continue;
        const qint64 size = fileInfo.isDir()
                                ? calculateDirectorySize(path, jobId)
                                : fileInfo.size();
        total += size;
        if (perSourceBytes)
            perSourceBytes->insert(path, size);
    }
    return total;
}

qint64 FileWorkerThread::calculateDirectorySize(const QString &path, const QUuid &jobId)
{
    constexpr auto filter = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    // Check cancel/stop only every N entries to avoid lock churn on large trees.
    constexpr int kCancelCheckInterval = 64;
    int sinceCheck = 0;

    qint64 size = 0;

    for (const QFileInfo &entry : QDir(path).entryInfoList(filter)) {
        if (++sinceCheck >= kCancelCheckInterval) {
            sinceCheck = 0;

            if (!jobId.isNull()) {
                waitIfPaused(jobId);
                if (isCancelled(jobId))
                    return size;
            }

            QMutexLocker lock(&m_mutex);
            if (m_stop)
                return size;
        }

        if (entry.isSymLink())
            continue;

        if (entry.isDir())
            size += calculateDirectorySize(entry.absoluteFilePath(), jobId);
        else
            size += entry.size();
    }

    return size;
}

bool FileWorkerThread::isCancelled(const QUuid &id) const
{
    QMutexLocker lock(&m_mutex);
    return m_cancelled.contains(id);
}
