#include <QDir>
#include <QStorageInfo>
#include <filesystem>
#include <functional>
#include <string>
#include "fileSystemUtils.h"
#include "fileWorkerThread.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static constexpr qint64 kChunkSizeMax            = 1024 * 1024;   // 1 MiB
static constexpr qint64 kChunkSizeMin            = 64 * 1024;     // 64 KiB
// Without a speed limit the chunk grows with the measured speed, up to this.
// A larger read lets Windows read ahead further — on an SMB share that keeps
// more requests in flight instead of waiting one round trip per MiB.
static constexpr qint64 kChunkSizeUnlimitedMax   = 16 * 1024 * 1024;  // 16 MiB
// Unlimited: one chunk should take about this long — short enough for pause to react quickly.
static constexpr int    kChunkTargetMs            = 500;

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

// Symlinks and NTFS junctions are never followed — see isLink() in fileSystemUtils.h.

// ".part" file the job leaves out (View → Hide .part Files). Size scan, copy
// and move all use it, so the progress math stays correct.
bool isSkippedPartFile(const QFileInfo &info, const FileJob &job) {
    return job.skipPartFiles && !info.isDir() && isPartFileName(info.fileName());
}

// Remove the link itself, never its target. QFile::remove() fails on a
// junction; rmdir drops just the link.
bool removeLink(const QFileInfo &info) {
    return info.isJunction() ? QDir().rmdir(info.absoluteFilePath())
                             : QFile::remove(info.absoluteFilePath());
}

// Read-only attribute of a file. Folders are ignored on purpose: Windows sets
// the flag on many folders (Desktop, Documents, …) only to mark them as
// customized, and Explorer ignores it there too.
bool isReadOnlyFile(const QFileInfo &info) {
#ifdef Q_OS_WIN
    if (info.isDir())
        return false;
    const std::wstring nativePath = QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY);
#else
    Q_UNUSED(info);
    return false;   // POSIX: deleting depends on the folder's permissions, not the file's
#endif
}

// Clear the read-only attribute — QFile::remove() fails on a read-only file.
void clearReadOnly(const QString &path) {
#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(nativePath.c_str(), attributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
#else
    Q_UNUSED(path);
#endif
}

// Rename that never copies. QFile::rename() (and MoveFile underneath) copy a
// file to another volume silently — blocking, without progress, and deaf to
// cancel, so closing the app kept the process alive until e.g. a large file
// from a NAS was through. Across volumes this fails instead, and moveSource()
// falls back to the chunked copy.
bool renameWithoutCopy(const QString &src, const QString &dst) {
#ifdef Q_OS_WIN
    const std::wstring nativeSrc = QDir::toNativeSeparators(src).toStdWString();
    const std::wstring nativeDst = QDir::toNativeSeparators(dst).toStdWString();
    // No MOVEFILE_COPY_ALLOWED — another volume fails with ERROR_NOT_SAME_DEVICE.
    return MoveFileExW(nativeSrc.c_str(), nativeDst.c_str(), 0) != 0;
#else
    // TODO(Linux): Windows API above — check this branch when porting.
    // std::filesystem::rename() maps to rename(2), which fails with EXDEV across
    // devices and never copies; verify that on a CIFS / NFS mount.
    std::error_code ec;
    std::filesystem::rename(src.toStdWString(), dst.toStdWString(), ec);
    return !ec;
#endif
}

// Decides for one read-only file whether it may be deleted (true) or is kept (false).
using ReadOnlyFilter = std::function<bool(const QString &)>;

// For cleanup the user already agreed to (overwritten originals, moved sources).
const ReadOnlyFilter kDeleteReadOnly = [](const QString &) { return true; };

// Delete one file. A read-only file is only deleted if @p allowReadOnly agrees;
// a file kept that way counts as success — the user chose to keep it.
// @p removedCount (optional) counts what was actually deleted.
bool removeFile(const QFileInfo &info, const ReadOnlyFilter &allowReadOnly, int *removedCount) {
    const QString path = info.absoluteFilePath();
    if (isReadOnlyFile(info)) {
        if (!allowReadOnly(path))
            return true;
        clearReadOnly(path);
    }
    const bool removed = QFile::remove(path);
    if (removed && removedCount)
        ++*removedCount;
    return removed;
}

// Remove a link and count it in @p removedCount (optional).
bool removeCountedLink(const QFileInfo &info, int *removedCount) {
    const bool removed = removeLink(info);
    if (removed && removedCount)
        ++*removedCount;
    return removed;
}

// Recursive delete that removes links instead of descending into them —
// QDir::removeRecursively() follows junctions. @p isCancelled is polled per
// entry, @p allowReadOnly is asked for every read-only file. A folder that
// still holds kept read-only files stays, without counting as an error.
// @p removedCount (optional) counts every file, link and folder deleted.
bool removeTree(const QString &path, const std::function<bool()> &isCancelled,
                const ReadOnlyFilter &allowReadOnly, int *removedCount = nullptr) {
    const QFileInfo info(path);

    if (!info.exists() && !isLink(info))
        return true;

    if (isLink(info))
        return removeCountedLink(info, removedCount);
    if (!info.isDir())
        return removeFile(info, allowReadOnly, removedCount);

    for (const QFileInfo &entry : QDir(path).entryInfoList(kAllEntries)) {
        if (isCancelled())
            return false;

        bool ok = false;
        if (isLink(entry))
            ok = removeCountedLink(entry, removedCount);
        else if (entry.isDir())
            ok = removeTree(entry.absoluteFilePath(), isCancelled, allowReadOnly, removedCount);
        else
            ok = removeFile(entry, allowReadOnly, removedCount);

        if (!ok)
            return false;
    }

    // Read-only files the user chose to keep leave the folder non-empty.
    if (!QDir(path).isEmpty(kAllEntries))
        return true;
    const bool removed = QDir().rmdir(path);
    if (removed && removedCount)
        ++*removedCount;
    return removed;
}

// Cleanup that must finish — no cancel check, read-only files included.
bool removeTreeForced(const QString &path) {
    return removeTree(path, [] { return false; }, kDeleteReadOnly);
}

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
    // The original may be read-only (copyMetadata() carries the flag over)
    // or a folder of the same name — a plain QFile::remove() left it behind.
    void dropBackup() {
        if (!backupPath.isEmpty() && !removeTreeForced(backupPath)) {
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

// Folder onto an existing folder of the same name: copied / moved into it
// without asking — only files that exist on both sides are asked about, like
// Explorer. Not when source and destination are the same folder: merging a
// folder into itself would overwrite every file with itself.
bool mergesIntoExistingFolder(const QFileInfo &srcInfo, const QString &dst) {
    const QFileInfo dstInfo(dst);
    return srcInfo.isDir() && !isLink(srcInfo) && dstInfo.isDir() && !isLink(dstInfo)
           && !pathEquals(srcInfo.absoluteFilePath(), dst);
}

// Consistent "<verb> \"<file>\": <reason>" error messages.
QString fileError(const QString &verb, const QString &path, const QString &reason) {
    return QStringLiteral("%1 \"%2\": %3").arg(verb, QFileInfo(path).fileName(), reason);
}

// "copied" / "moved" / "deleted" — for the job's failure summary.
QString pastTense(FileJob::Type type) {
    switch (type) {
    case FileJob::Type::Copy:   return QObject::tr("copied");
    case FileJob::Type::Move:   return QObject::tr("moved");
    case FileJob::Type::Delete: return QObject::tr("deleted");
    }
    return {};
}

// Sources of one job that succeeded / failed — summarised in job.errorMessage
// once all sources are through.
struct SourceTally {
    int         succeeded = 0;
    QStringList failedNames;

    // The first failure without a more specific reason names the source.
    void addFailure(FileJob &job, const QString &name) {
        failedNames << name;
        if (job.errorMessage.isEmpty())
            job.errorMessage = QObject::tr("%1 failed: \"%2\"").arg(job.typeString(), name);
    }

    // "2 copied, 1 failed (a.txt) — <first reason>". The specific reason, set by
    // the first failure, stays after the summary instead of being overwritten.
    void writeSummary(FileJob &job) const {
        if (failedNames.isEmpty())
            return;
        const QString failed  = QString::number(failedNames.size());
        const QString names   = failedNames.join(QStringLiteral(", "));
        const QString summary = succeeded > 0
            ? QObject::tr("%1 %2, %3 failed (%4)").arg(QString::number(succeeded),
                                                      pastTense(job.type), failed, names)
            : QObject::tr("All %1 item(s) failed (%2)").arg(failed, names);
        job.errorMessage = job.errorMessage.isEmpty()
                               ? summary
                               : summary + QStringLiteral(" — ") + job.errorMessage;
    }
};

// If @p dst exists, rename it to a backup path and register that path on
// @p guard so it is restored on failure. Returns false and fills
// @p job.errorMessage when the rename fails. The guard only gets @p dst as
// its target once the original is safely out of the way — before, a failed
// rename made the guard delete the original it could not move aside.
bool prepareDestination(const QString &dst, FileJob &job, FileReplaceGuard &guard) {
    const QFileInfo dstInfo(dst);
    if (!dstInfo.exists() && !isLink(dstInfo)) {
        guard.setTarget(dst);
        return true;
    }

    // dst exists — opening WriteOnly would truncate it immediately, so any
    // mid-copy error would leave the user with an empty/partial file.
    // Move it aside first; the guard restores it if we fail.
    const QString backupPath = makeBackupPath(dst);
    if (!QDir().rename(dst, backupPath)) {
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = fileError(QObject::tr("Cannot replace"), dst,
                                         QObject::tr("rename failed"));
        }
        return false;
    }
    guard.setTarget(dst);
    guard.setBackup(backupPath);
    return true;
}

#ifdef Q_OS_WIN
// Called by CopyFileExW after every chunk (and at each stream start).
struct NativeCopyContext {
    /// @p stream 1 = file content, higher = alternate data streams.
    /// Returns false to cancel the copy.
    std::function<bool(DWORD stream, qint64 streamBytes)> onProgress;
};

DWORD CALLBACK nativeCopyProgress(LARGE_INTEGER /*totalFileSize*/, LARGE_INTEGER /*totalTransferred*/,
                                  LARGE_INTEGER /*streamSize*/, LARGE_INTEGER streamTransferred,
                                  DWORD streamNumber, DWORD /*callbackReason*/,
                                  HANDLE /*sourceFile*/, HANDLE /*destinationFile*/, LPVOID data)
{
    auto *context = static_cast<NativeCopyContext *>(data);
    return context->onProgress(streamNumber, streamTransferred.QuadPart) ? PROGRESS_CONTINUE
                                                                         : PROGRESS_CANCEL;
}
#endif

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
        cancelPendingIo();   // a read from a dead NAS would block wait() below
        m_condition.wakeAll();
        m_pauseCondition.wakeAll();
    }

    {
        // Unblock the worker if it is parked on the conflict or read-only dialog handshake.
        QMutexLocker lock(&m_conflictMutex);
        m_conflictPending = false;
        m_readOnlyPending = false;
        m_conflictCondition.wakeAll();
    }
    wait();

#ifdef Q_OS_WIN
    if (m_threadHandle)
        CloseHandle(static_cast<HANDLE>(m_threadHandle));
#endif
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
    if (id == m_runningJobId)
        cancelPendingIo();
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

    cancelPendingIo();
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

void FileWorkerThread::resolveReadOnly(FileJob::ReadOnlyAction action, bool applyToAll)
{
    QMutexLocker lock(&m_conflictMutex);
    m_readOnlyResult     = action;
    m_readOnlyApplyToAll = applyToAll;
    m_readOnlyPending    = false;
    m_conflictCondition.wakeOne();
}


void FileWorkerThread::run()
{
#ifdef Q_OS_WIN
    {
        // CancelSynchronousIo() needs a real handle with THREAD_TERMINATE access —
        // GetCurrentThread() is only a pseudo handle.
        QMutexLocker lock(&m_mutex);
        m_threadHandle = OpenThread(THREAD_TERMINATE, FALSE, GetCurrentThreadId());
    }
#endif

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
            emit jobFinished(job.id, FileJob::Status::Cancelled, {}, /*unchanged=*/true);
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
    case FileJob::Type::Move: {
        emit jobScanning(job.id);
        // Per-source sizes feed the per-file rows in the job queue, let a skipped
        // source still advance the progress, and let moveSource() bump progress
        // on a same-volume rename without re-walking the source tree.
        const bool isMove = job.type == FileJob::Type::Move;
        // Move also notes which sources hold read-only files — the user is asked
        // before those are moved.
        QSet<QString> *readOnlySources = isMove ? &job.readOnlySources : nullptr;
        // Move with .part files left out also notes the folders that hold them —
        // those cannot be renamed as a whole.
        QSet<QString> *partFileDirs = isMove && job.skipPartFiles ? &job.partFileDirs : nullptr;

        job.bytesTotal = calculateTotalBytes(job, &job.bytesPerSource, readOnlySources, partFileDirs);
        break;
    }
    case FileJob::Type::Delete:
        job.bytesTotal = job.sources.size();
        break;
    }

    // Per-source sizes let the job queue show a remaining time for every file.
    QList<qint64> sourceBytes;
    sourceBytes.reserve(job.sources.size());
    for (const QString &source : std::as_const(job.sources))
        sourceBytes << (job.type == FileJob::Type::Delete ? 1 : job.bytesPerSource.value(source, 0));
    emit jobStarted(job.id, job.bytesTotal, sourceBytes);

    // Before the first byte is written — otherwise the copy runs until the disk
    // is full and leaves a partial set of files at the destination.
    if (job.type != FileJob::Type::Delete && !isCancelled(job.id) && !hasEnoughSpace(job)) {
        finalizeJob(job);
        return;
    }

    switch (job.type) {
    case FileJob::Type::Copy:   runCopyJob(job);   break;
    case FileJob::Type::Move:   runMoveJob(job);   break;
    case FileJob::Type::Delete: runDeleteJob(job); break;
    }
}

bool FileWorkerThread::hasEnoughSpace(FileJob &job)
{
    // Runs on the worker thread — QStorageInfo may block on a network destination.
    const QStorageInfo destination(job.destination);
    if (!destination.isValid() || !destination.isReady())
        return true;   // unknown — the copy itself will report a write error

    qint64 needed = job.bytesTotal;
    if (job.type == FileJob::Type::Move) {
        // A move on the same volume is a rename and needs no space.
        needed = 0;
        for (auto it = job.bytesPerSource.cbegin(); it != job.bytesPerSource.cend(); ++it) {
            if (!pathEquals(QStorageInfo(it.key()).rootPath(), destination.rootPath()))
                needed += it.value();
        }
    }

    // bytesAvailable() honours disk quotas — what this user can actually write.
    const qint64 available = destination.bytesAvailable();
    if (needed <= available)
        return true;

    job.errorMessage = tr("Not enough space on %1: %2 needed, %3 free")
                           .arg(QDir::toNativeSeparators(destination.rootPath()),
                                formatSize(needed), formatSize(available));
    emit insufficientSpace(job.id, job.destination, needed, available);
    return false;
}

// Only called once per job, after all work has completed — never mid-operation.
// Partial-file cleanup is handled by FileReplaceGuard inside copyFile().
void FileWorkerThread::finalizeJob(FileJob &job)
{
    const auto addWarning = [&job](const QString &warning) {
        if (!job.warningMessage.isEmpty())
            job.warningMessage += QStringLiteral("; ");
        job.warningMessage += warning;
    };

    if (job.symlinksSkipped > 0)
        addWarning(tr("%1 link(s) skipped (symlinks / junctions)").arg(job.symlinksSkipped));
    // Skipped on a destination conflict or kept as read-only.
    if (job.itemsSkipped > 0)
        addWarning(tr("%1 item(s) skipped").arg(job.itemsSkipped));
    if (job.readOnlyCopied > 0)
        addWarning(tr("%1 read-only item(s) copied instead of moved").arg(job.readOnlyCopied));

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

    // Skipped links are kept apart: the user did not choose that, so a job
    // that only skipped links must stay visible with its warning.
    const bool unchanged = job.itemsChanged == 0 && job.symlinksSkipped == 0;
    emit jobFinished(job.id, job.status, finalMessage, unchanged);

    QMutexLocker lock(&m_mutex);
    m_cancelled.remove(job.id);
    m_paused.remove(job.id);
    m_runningJobId      = QUuid();
    m_runningJobSources.clear();
}

void FileWorkerThread::skipSource(FileJob &job, int index, qint64 sourceBytes, qint64 &bytesDone)
{
    // Nothing is written, but the bytes count as processed — otherwise the job
    // would end below 100 % (or, before, jump to 100 % without copying anything).
    ++job.itemsSkipped;
    bytesDone += sourceBytes;
    emit jobProgress(job.id, bytesDone);
    emit sourceFinished(job.id, index, FileJob::Status::Skipped);
}

void FileWorkerThread::reportSourceResult(FileJob &job, int index, bool success, int skippedBefore)
{
    FileJob::Status status = FileJob::Status::Failed;
    if (success)
        // Files inside a folder were skipped on conflict — the folder is only partly copied.
        status = job.itemsSkipped > skippedBefore ? FileJob::Status::DoneWithWarnings
                                                  : FileJob::Status::Done;
    else if (isCancelled(job.id))
        status = FileJob::Status::Cancelled;

    emit sourceFinished(job.id, index, status);
}

bool FileWorkerThread::skipLinkOrPartFile(FileJob &job, int index, const QFileInfo &sourceInfo)
{
    // Links are skipped consistently at every recursion level — calculateTotalBytes()
    // ignores them too, so the progress math stays correct. (A cross-volume move
    // of a junction would copy and then delete its target.)
    if (isLink(sourceInfo)) {
        ++job.symlinksSkipped;
        emit sourceFinished(job.id, index, FileJob::Status::Skipped);
        return true;
    }

    // Left out on purpose — stays where it is, no warning: the user asked for it.
    if (isSkippedPartFile(sourceInfo, job)) {
        emit sourceFinished(job.id, index, FileJob::Status::Skipped);
        return true;
    }
    return false;
}

QString FileWorkerThread::destinationFor(const QFileInfo &sourceInfo, FileJob &job,
                                         FileJob::ConflictAction &conflictAction)
{
    const QString dst = QDir(job.destination).filePath(sourceInfo.fileName());
    return mergesIntoExistingFolder(sourceInfo, dst)
               ? dst
               : resolveDestinationConflict(dst, job.id, conflictAction);
}

void FileWorkerThread::runCopyJob(FileJob &job)
{
    qint64 bytesDone = 0;
    FileJob::ConflictAction conflictAction = FileJob::ConflictAction::Ask;
    SourceTally tally;

    for (int index = 0; index < job.sources.size(); ++index) {
        if (isCancelledAfterPause(job.id))
            break;

        const QString  &sourcePath  = job.sources.at(index);
        const QFileInfo sourceInfo(sourcePath);
        const qint64    sourceBytes = job.bytesPerSource.value(sourcePath, 0);
        emit sourceStarted(job.id, index, sourceBytes);

        if (skipLinkOrPartFile(job, index, sourceInfo))
            continue;

        const QString destPath = destinationFor(sourceInfo, job, conflictAction);
        if (destPath.isEmpty()) {
            if (isCancelled(job.id)) break;   // "Cancel Job" in the conflict dialog
            skipSource(job, index, sourceBytes, bytesDone);
            continue;
        }

        const int  skippedBefore = job.itemsSkipped;
        const bool success = sourceInfo.isDir()
                                 ? copyRecursive(sourcePath, destPath, job, bytesDone, conflictAction)
                                 : copyFile(sourcePath, destPath, job, bytesDone);
        reportSourceResult(job, index, success, skippedBefore);

        if (success)
            ++tally.succeeded;
        else if (!isCancelled(job.id))
            tally.addFailure(job, sourceInfo.fileName());
    }

    tally.writeSummary(job);
    finalizeJob(job);
}

void FileWorkerThread::runMoveJob(FileJob &job)
{
    qint64 bytesDone = 0;
    FileJob::ConflictAction conflictAction = FileJob::ConflictAction::Ask;
    SourceTally tally;

    for (int index = 0; index < job.sources.size(); ++index) {
        if (isCancelledAfterPause(job.id))
            break;

        const QString  &src         = job.sources.at(index);
        const QFileInfo sourceInfo(src);
        const qint64    sourceBytes = job.bytesPerSource.value(src, 0);
        emit sourceStarted(job.id, index, sourceBytes);

        if (skipLinkOrPartFile(job, index, sourceInfo))
            continue;

        // Read-only files are protected against changes — ask before moving them:
        // move anyway, only copy (original stays), or skip.
        const FileJob::ReadOnlyAction readOnly = job.readOnlySources.contains(src)
                                                     ? askReadOnly(job, src, /*isMove=*/true)
                                                     : FileJob::ReadOnlyAction::Proceed;
        if (isCancelled(job.id)) break;   // "Cancel Job" in the read-only dialog
        if (readOnly == FileJob::ReadOnlyAction::Skip) {
            skipSource(job, index, sourceBytes, bytesDone);
            continue;
        }
        const bool copyOnly = readOnly == FileJob::ReadOnlyAction::CopyOnly;

        // An existing folder of the same name is merged (moveSource()), not asked about.
        const QString dst = destinationFor(sourceInfo, job, conflictAction);
        if (dst.isEmpty()) {
            if (isCancelled(job.id)) break;   // "Cancel Job" in the conflict dialog
            skipSource(job, index, sourceBytes, bytesDone);
            continue;
        }

        const int skippedBefore = job.itemsSkipped;
        bool moved = false;
        if (copyOnly) {
            moved = sourceInfo.isDir() ? copyRecursive(src, dst, job, bytesDone, conflictAction)
                                       : copyFile(src, dst, job, bytesDone);
            if (moved)
                ++job.readOnlyCopied;
        } else {
            moved = moveSource(src, dst, sourceInfo, sourceBytes, job, bytesDone, conflictAction);
        }

        if (copyOnly && moved)
            emit sourceFinished(job.id, index, FileJob::Status::DoneWithWarnings);   // original stays
        else
            reportSourceResult(job, index, moved, skippedBefore);

        if (moved)
            ++tally.succeeded;
        else if (!isCancelled(job.id))
            tally.addFailure(job, sourceInfo.fileName());
    }

    tally.writeSummary(job);
    finalizeJob(job);
}

void FileWorkerThread::runDeleteJob(FileJob &job)
{
    qint64 processed = 0;
    SourceTally tally;

    for (int index = 0; index < job.sources.size(); ++index) {
        if (isCancelledAfterPause(job.id))
            break;

        const QString  &path = job.sources.at(index);
        const QFileInfo entry(path);
        // Delete progress counts items, not bytes — each source is one unit.
        emit sourceStarted(job.id, index, 1);

        // Links are removed themselves, never their target; files and folders
        // go through the cancellable delete, which asks for read-only files.
        const int skippedBefore = job.itemsSkipped;
        bool deleted = false;
        if (isLink(entry))
            deleted = removeCountedLink(entry, &job.itemsChanged);
        else
            deleted = removeRecursive(path, job, /*confirmReadOnly=*/true);

        // A read-only file the user chose to keep — skipped, not deleted.
        if (deleted && !entry.isDir() && job.itemsSkipped > skippedBefore) {
            emit sourceFinished(job.id, index, FileJob::Status::Skipped);
            ++processed;
            emit jobProgress(job.id, processed);
            continue;
        }

        reportSourceResult(job, index, deleted, skippedBefore);

        if (deleted)
            ++tally.succeeded;
        else if (!isCancelled(job.id))
            tally.addFailure(job, entry.fileName());

        ++processed;
        emit jobProgress(job.id, processed);
    }

    tally.writeSummary(job);
    finalizeJob(job);
}

bool FileWorkerThread::removeRecursive(const QString &path, FileJob &job, bool confirmReadOnly)
{
    // Delete jobs ask per read-only file; a moved source was already agreed to as a whole.
    const ReadOnlyFilter allowReadOnly = !confirmReadOnly ? kDeleteReadOnly
                                                          : ReadOnlyFilter([this, &job](const QString &file) {
        if (askReadOnly(job, file, /*isMove=*/false) == FileJob::ReadOnlyAction::Proceed)
            return true;
        if (!isCancelled(job.id))
            ++job.itemsSkipped;   // kept on purpose — "Cancel Job" is no skip
        return false;
    });

    // isCancelled() covers stop too — the destructor cancels the running job.
    return removeTree(path, [this, &job] { return isCancelled(job.id); }, allowReadOnly,
                      &job.itemsChanged);
}

FileJob::ReadOnlyAction FileWorkerThread::askReadOnly(FileJob &job, const QString &path, bool isMove)
{
    // A previous "Apply to all" already decided for this job.
    if (job.readOnlyAction != FileJob::ReadOnlyAction::Ask)
        return job.readOnlyAction;

    FileJob::ReadOnlyAction resolved;
    bool applyToAll;
    {
        QMutexLocker lock(&m_conflictMutex);
        m_readOnlyPending = true;

        lock.unlock();
        emit readOnlyDetected(job.id, path, isMove);
        lock.relock();

        while (m_readOnlyPending && !m_stop)
            m_conflictCondition.wait(&m_conflictMutex);

        if (m_stop) return FileJob::ReadOnlyAction::Skip;   // shutting down

        resolved   = m_readOnlyResult;
        applyToAll = m_readOnlyApplyToAll;
    }

    if (applyToAll)
        job.readOnlyAction = resolved;
    return resolved;
}

bool FileWorkerThread::mergeMoveDirectory(const QString &src, const QString &dst, FileJob &job,
                                          qint64 &bytesDone, FileJob::ConflictAction &conflictAction)
{
    bool allOk = true;

    for (const QFileInfo &entry : QDir(src).entryInfoList(kAllEntries)) {
        if (isCancelledAfterPause(job.id))
            return false;

        // Links stay in the source, as on copy.
        if (isLink(entry)) {
            ++job.symlinksSkipped;
            continue;
        }
        // .part files stay in the source too — the folder is kept for them.
        if (isSkippedPartFile(entry, job))
            continue;

        const QString   srcPath  = entry.absoluteFilePath();
        const QString   dstPath  = QDir(dst).filePath(entry.fileName());
        const QFileInfo dstEntry(dstPath);
        // A subfolder that exists on both sides is merged as well — its files
        // count their own bytes, so its size is only needed when it moves whole.
        const bool mergeInto  = entry.isDir() && dstEntry.isDir() && !isLink(dstEntry);
        qint64     entryBytes = 0;
        if (!mergeInto)
            entryBytes = entry.isDir() ? calculateDirectorySize(srcPath, job) : entry.size();

        QString resolved = dstPath;
        if (!mergeInto) {
            resolved = resolveDestinationConflict(dstPath, job.id, conflictAction);
            if (resolved.isEmpty()) {
                if (isCancelled(job.id))
                    return false;
                // Skipped — stays in the source; its bytes count as processed.
                ++job.itemsSkipped;
                bytesDone += entryBytes;
                emit jobProgress(job.id, bytesDone);
                continue;
            }
        }

        if (!moveSource(srcPath, resolved, entry, entryBytes, job, bytesDone, conflictAction)) {
            if (isCancelled(job.id))
                return false;
            allOk = false;   // hard errors only flip the flag, the siblings still move
        }
    }

    // Skipped items and links keep the source folder alive — remove it only when empty.
    if (QDir(src).isEmpty(kAllEntries) && QDir().rmdir(src))
        ++job.itemsChanged;
    return allOk;
}

bool FileWorkerThread::moveSource(const QString &src, const QString &dst, const QFileInfo &srcInfo,
                                  qint64 srcBytes, FileJob &job,
                                  qint64 &bytesDone, FileJob::ConflictAction &conflictAction)
{
    // Folder onto an existing folder: merge like Explorer and like Copy does.
    // Replacing it as a whole deleted every file that only the target had.
    const QFileInfo dstInfo(dst);
    if (srcInfo.isDir() && !isLink(srcInfo) && dstInfo.isDir() && !isLink(dstInfo))
        return mergeMoveDirectory(src, dst, job, bytesDone, conflictAction);

    // A folder holding .part files that are left out: create the target and move
    // entry by entry, so they stay behind. A rename would take them along, and
    // moveByCopy() would delete them with the source.
    if (srcInfo.isDir() && !isLink(srcInfo) && !dstInfo.exists() && !isLink(dstInfo)
        && job.partFileDirs.contains(src)) {
        if (!QDir().mkdir(dst)) {
            if (job.errorMessage.isEmpty()) {
                job.errorMessage = tr("Move failed: \"%1\": cannot create directory")
                                       .arg(QDir::toNativeSeparators(dst));
            }
            return false;
        }
        ++job.itemsChanged;
        return mergeMoveDirectory(src, dst, job, bytesDone, conflictAction);
    }

    // resolveDestinationConflict handed us a dst that the user already agreed to overwrite
    // (otherwise it would have returned an empty string or a unique name).
    // Stash the existing target under a temp name first — if the rename or the
    // cross-device copy then fails, we can roll the original back in. Deleting
    // dst up front would lose data on any failure between delete and rename.
    QString stashPath;
    if (dstInfo.exists() || isLink(dstInfo)) {
        stashPath = makeBackupPath(dst);
        if (!QDir().rename(dst, stashPath)) {
            job.errorMessage = tr("Move failed: could not replace \"%1\"")
                                   .arg(QFileInfo(dst).fileName());
            return false;
        }
    }

    // Skips the cancel check on purpose — once invoked, cleanup must finish.
    // The overwritten original may be read-only — the user agreed to replace it.
    const auto removeFileOrDir = [](const QString &path) {
        removeTreeForced(path);
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

    // Same volume: the OS just relinks the entry. Another volume (NAS → local disk)
    // fails here and goes through moveByCopy() — with progress and cancel.
    if (renameWithoutCopy(src, dst)) {
        discardStash();
        ++job.itemsChanged;
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

/// @brief Return a chunk size adapted to the speed limit or the measured speed.
///
///  - Uncapped:    ~500 ms of data at the measured speed, 1–16 MB — a larger
///                 read lets Windows read ahead further, so an SMB share gets
///                 more requests in flight (closer to Explorer's throughput)
///  - Limited:     chunk sized so we write ~10 bursts per second (100 ms cycle)
///                 → each burst is short, disk I/O looks smoother in Task Manager
///  - Minimum:     64 KB — below this, syscall overhead hurts performance
qint64 FileWorkerThread::adaptiveChunkSize(double measuredBps) const
{
    QMutexLocker lock(&m_mutex);

    if (m_speedLimitBps <= 0.0) {
        const qint64 bytesPerTarget = qint64(measuredBps * kChunkTargetMs / 1000.0);
        return qBound(kChunkSizeMax, bytesPerTarget, kChunkSizeUnlimitedMax);
    }

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
    // prepareDestination() sets the target only once dst may be removed.
    FileReplaceGuard guard;
    QFile destFile(dst);

    const auto setErrorOnce = [&](const QString &msg) {
        if (job.errorMessage.isEmpty())
            job.errorMessage = msg;
    };

    if (!prepareDestination(dst, job, guard))
        return false;

#ifdef Q_OS_WIN
    // Without a speed limit Windows' own copy engine does the work — it keeps
    // several requests in flight on SMB and overlaps read and write (Explorer's
    // throughput; the chunk loop below reached ~10 of ~11.3 MiB/s on 100 Mbit).
    // With a limit the chunk loop stays: it writes in small, even bursts.
    bool speedLimited;
    { QMutexLocker lock(&m_mutex); speedLimited = m_speedLimitBps > 0.0; }
    if (!speedLimited) {
        qint64 copiedBytes = 0;
        if (!copyFileNative(src, dst, job, bytesDone, copiedBytes))
            return false;

        // Fresh QFileInfo — asks the server what actually arrived.
        const qint64 destSize = QFileInfo(dst).size();
        if (destSize != copiedBytes) {
            setErrorOnce(fileError(tr("Write error on"), dst,
                                   tr("incomplete (%1 of %2 bytes)")
                                       .arg(destSize).arg(copiedBytes)));
            clearReadOnly(dst);   // CopyFileExW carried the flag over — the guard must delete it
            return false;
        }

        // Attributes and timestamps were copied by CopyFileExW — no copyMetadata().
        guard.commit();
        ++job.itemsChanged;
        return true;
    }
#endif

    if (!sourceFile.open(QIODevice::ReadOnly)) {
        setErrorOnce(fileError(tr("Cannot read"), src, sourceFile.errorString()));
        return false;
    }
    if (!destFile.open(QIODevice::WriteOnly)) {
        setErrorOnce(fileError(tr("Cannot write"), dst, destFile.errorString()));
        return false;
    }

    SpeedTracker speed;
    speed.start();

    QElapsedTimer signalTimer;
    signalTimer.start();

    // Re-evaluate chunk size periodically so a mid-copy speed-limit change
    // takes effect inside the same file, and so the chunk grows with the
    // measured speed when there is no limit. Once per ~250 ms is plenty.
    qint64 chunkSize = adaptiveChunkSize(0.0);
    QElapsedTimer chunkSizeTimer;
    chunkSizeTimer.start();
    QElapsedTimer fileTimer;
    fileTimer.start();

    // One buffer for the whole file — read(qint64) would allocate up to 16 MB per chunk.
    QByteArray buffer;
    qint64 fileBytes = 0;   // written to this file — checked against the result below

    while (!sourceFile.atEnd()) {
        waitIfPaused(job.id);
        if (isCancelled(job.id))
            return false;

        if (chunkSizeTimer.elapsed() >= kChunkReevalIntervalMs) {
            const qint64 elapsedMs = fileTimer.elapsed();
            chunkSize = adaptiveChunkSize(elapsedMs > 0 ? fileBytes * 1000.0 / elapsedMs : 0.0);
            chunkSizeTimer.restart();
        }
        if (buffer.size() < chunkSize)
            buffer.resize(chunkSize);

        // Read and write may block for the SMB timeout on a dead NAS — cancel
        // aborts them (cancelPendingIo()); that is no error, only a cancel.
        if (!beginCancellableIo(job.id))
            return false;
        const qint64 bytesRead = sourceFile.read(buffer.data(), chunkSize);
        endCancellableIo();
        if (bytesRead <= 0) {
            if (bytesRead < 0 || sourceFile.error() != QFileDevice::NoError) {
                if (!isCancelled(job.id))
                    setErrorOnce(fileError(tr("Read error on"), src,
                                           sourceFile.errorString()));
                return false;
            }
            break;
        }

        if (!beginCancellableIo(job.id))
            return false;
        const qint64 written = destFile.write(buffer.constData(), bytesRead);
        endCancellableIo();
        if (written != bytesRead) {
            if (!isCancelled(job.id))
                setErrorOnce(fileError(tr("Write error on"), dst,
                                       destFile.errorString()));
            return false;
        }

        fileBytes += bytesRead;
        bytesDone += bytesRead;
        speed.addBytes(bytesRead);
        applySpeedThrottle(speed);

        if (signalTimer.elapsed() >= kProgressSignalIntervalMs || sourceFile.atEnd()) {
            emit jobProgress(job.id, bytesDone);
            signalTimer.restart();
        }

        speed.resetIfWindowExpired();
    }

    // A network destination may report write errors only now — SMB buffers the
    // writes (full disk on the NAS, dropped connection). Ignoring them let a Move
    // delete the source although the copy was incomplete. Closed explicitly
    // before touching metadata so Windows lets us update the file.
    if (!destFile.flush()) {
        setErrorOnce(fileError(tr("Write error on"), dst, destFile.errorString()));
        return false;
    }
    destFile.close();
    if (destFile.error() != QFileDevice::NoError) {
        setErrorOnce(fileError(tr("Write error on"), dst, destFile.errorString()));
        return false;
    }

    // Fresh QFileInfo — asks the server what actually arrived.
    const qint64 destSize = QFileInfo(dst).size();
    if (destSize != fileBytes) {
        setErrorOnce(fileError(tr("Write error on"), dst,
                               tr("incomplete (%1 of %2 bytes)")
                                   .arg(destSize).arg(fileBytes)));
        return false;
    }

    // Commit before metadata: once the content is on disk the copy is "done".
    // Any later step (mtime, permissions) is best-effort and must not be able
    // to trigger the guard destructor and wipe a valid file.
    guard.commit();
    ++job.itemsChanged;

    copyMetadata(src, dst);
    return true;
}

#ifdef Q_OS_WIN
bool FileWorkerThread::copyFileNative(const QString &src, const QString &dst,
                                      FileJob &job, qint64 &bytesDone, qint64 &copiedBytes)
{
    SpeedTracker speed;
    speed.start();

    QElapsedTimer signalTimer;
    signalTimer.start();

    NativeCopyContext context;
    context.onProgress = [&](DWORD stream, qint64 streamBytes) {
        // Alternate data streams (e.g. Zone.Identifier) follow the content and
        // are not part of the scanned size — only the content counts.
        if (stream == 1 && streamBytes > copiedBytes) {
            const qint64 delta = streamBytes - copiedBytes;
            copiedBytes = streamBytes;
            bytesDone  += delta;
            speed.addBytes(delta);
            if (signalTimer.elapsed() >= kProgressSignalIntervalMs) {
                emit jobProgress(job.id, bytesDone);
                signalTimer.restart();
            }
        }

        // Between two chunks no I/O is pending — pause and throttle here. A
        // limit set mid-file applies at the engine's chunk size (coarser bursts).
        endCancellableIo();
        waitIfPaused(job.id);
        if (isCancelled(job.id))
            return false;
        applySpeedThrottle(speed);
        speed.resetIfWindowExpired();
        return beginCancellableIo(job.id);
    };

    // A dead NAS can block inside CopyFileExW — cancel aborts it (cancelPendingIo()).
    if (!beginCancellableIo(job.id))
        return false;
    // dst was moved aside by prepareDestination() — FAIL_IF_EXISTS never hits an original.
    // ALLOW_DECRYPTED_DESTINATION: an EFS file onto a NAS, like the chunk loop does.
    const std::wstring nativeSrc = QDir::toNativeSeparators(src).toStdWString();
    const std::wstring nativeDst = QDir::toNativeSeparators(dst).toStdWString();
    const BOOL ok = CopyFileExW(nativeSrc.c_str(), nativeDst.c_str(),
                                nativeCopyProgress, &context, nullptr,
                                COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_ALLOW_DECRYPTED_DESTINATION);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    endCancellableIo();

    // CopyFileExW removes the partial file itself on failure and on cancel.
    if (!ok) {
        if (!isCancelled(job.id) && job.errorMessage.isEmpty())
            job.errorMessage = fileError(tr("Cannot copy"), src,
                                         qt_error_string(int(error)));
        return false;
    }

    emit jobProgress(job.id, bytesDone);
    return true;
}
#endif

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
    // Copying into an existing folder changes nothing by itself — only the files do.
    const bool created = !QFileInfo(dst).isDir();
    if (!QDir(dst).mkpath(".")) {
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = tr("Copy failed: \"%1\": cannot create directory")
                                   .arg(QDir::toNativeSeparators(dst));
        }
        return false;
    }
    if (created)
        ++job.itemsChanged;

    bool allOk = true;

    for (const QFileInfo &entry : QDir(src).entryInfoList(kAllEntries)) {
        if (isLink(entry)) {
            ++job.symlinksSkipped;
            continue;
        }
        // Left out on purpose (Hide .part Files) — the size scan skipped them too.
        if (isSkippedPartFile(entry, job))
            continue;

        if (isCancelledAfterPause(job.id))
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
        if (resolvedPath.isEmpty()) {
            if (isCancelled(job.id))
                return false;
            // User chose Skip — count the file as processed so the progress still
            // reaches 100 %; the job reports the skipped items as a warning.
            ++job.itemsSkipped;
            bytesDone += entry.size();
            emit jobProgress(job.id, bytesDone);
            continue;
        }

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
    // Read-only files were already agreed to in runMoveJob() — no second question.
    const bool deleted = removeRecursive(src, job, /*confirmReadOnly=*/false);

    if (!deleted) {
        // Cancel path: finalizeJob will mark the job Cancelled — leaving
        // errorMessage empty prevents a bogus "delete after copy" tooltip.
        if (isCancelled(job.id))
            return false;
        job.errorMessage = tr("Move failed: \"%1\": source could not be deleted after copy")
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
    if (!QFile::exists(dst) && !isLink(QFileInfo(dst)))
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

        // Overwriting a read-only file replaces protected content — the dialog says so.
        const QFileInfo dstInfo(dst);
        const bool readOnly = isReadOnlyFile(dstInfo);

        lock.unlock();
        emit conflictDetected(jobId, dstInfo.fileName(), dst, readOnly);
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

    // Timed wait on m_pauseCondition — woken early by pause/cancel/stop
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

qint64 FileWorkerThread::calculateTotalBytes(const FileJob &job,
                                             QHash<QString, qint64> *perSourceBytes,
                                             QSet<QString> *readOnlySources,
                                             QSet<QString> *partFileDirs)
{
    qint64 total = 0;
    for (const QString &path : std::as_const(job.sources)) {
        if (isCancelledAfterPause(job.id))
            break;

        const QFileInfo fileInfo(path);
        // Symlinks (and left-out .part files) are skipped everywhere in copy/move
        // (see runCopyJob / copyRecursive), so they must not contribute to
        // bytesTotal either — otherwise the progress bar would never reach 100%.
        if (isLink(fileInfo) || isSkippedPartFile(fileInfo, job))
            continue;
        bool containsReadOnly = false;
        const qint64 size = fileInfo.isDir()
                                ? calculateDirectorySize(path, job,
                                                         readOnlySources ? &containsReadOnly : nullptr,
                                                         partFileDirs)
                                : fileInfo.size();
        total += size;
        if (perSourceBytes)
            perSourceBytes->insert(path, size);
        if (readOnlySources && (containsReadOnly || isReadOnlyFile(fileInfo)))
            readOnlySources->insert(path);
    }
    return total;
}

qint64 FileWorkerThread::calculateDirectorySize(const QString &path, const FileJob &job,
                                                bool *containsReadOnly, QSet<QString> *partFileDirs)
{
    // Check cancel/stop only every N entries to avoid lock churn on large trees.
    constexpr int kCancelCheckInterval = 64;
    int sinceCheck = 0;

    qint64 size = 0;
    bool   holdsPartFile = false;   // a left-out .part file in here or below

    // kAllEntries, like copyRecursive() — without Hidden / System the total was
    // smaller than what gets copied, and the progress went past 100 %.
    for (const QFileInfo &entry : QDir(path).entryInfoList(kAllEntries)) {
        // Also before every subfolder — listing one over SMB is the slow part,
        // a cancel / quit must not wait for dozens more of them.
        if (++sinceCheck >= kCancelCheckInterval || entry.isDir()) {
            sinceCheck = 0;

            if (isCancelledAfterPause(job.id))
                return size;

            QMutexLocker lock(&m_mutex);
            if (m_stop)
                return size;
        }

        if (isLink(entry))
            continue;
        if (isSkippedPartFile(entry, job)) {
            holdsPartFile = true;
            continue;
        }

        if (entry.isDir()) {
            const QString subPath = entry.absoluteFilePath();
            size += calculateDirectorySize(subPath, job, containsReadOnly, partFileDirs);
            if (partFileDirs && partFileDirs->contains(subPath))
                holdsPartFile = true;
        } else {
            size += entry.size();
            if (containsReadOnly && !*containsReadOnly && isReadOnlyFile(entry))
                *containsReadOnly = true;
        }
    }

    // Keys are the paths moveSource() gets later — the same entryInfoList() strings.
    if (partFileDirs && holdsPartFile)
        partFileDirs->insert(path);
    return size;
}

bool FileWorkerThread::isCancelled(const QUuid &id) const
{
    QMutexLocker lock(&m_mutex);
    return m_cancelled.contains(id);
}

bool FileWorkerThread::isCancelledAfterPause(const QUuid &id)
{
    if (isCancelled(id))
        return true;
    waitIfPaused(id);
    return isCancelled(id);
}

// ── Cancellable I/O ───────────────────────────────────────────────────────────
//
// A read or write on a NAS that went away blocks until the SMB timeout (60 s or
// more) — cancel and quit waited for it. Only the copy loop's read/write is
// marked cancellable: cleanup (rollback, backup removal) must never be aborted.
//
bool FileWorkerThread::beginCancellableIo(const QUuid &id)
{
    QMutexLocker lock(&m_mutex);
    // Checked under the same lock — a cancel after this point aborts the I/O.
    if (m_cancelled.contains(id))
        return false;
    m_ioCancellable = true;
    return true;
}

void FileWorkerThread::endCancellableIo()
{
    // Waits for a running cancelPendingIo() (it holds m_mutex), so the cancel
    // can never hit the next, non-cancellable call.
    QMutexLocker lock(&m_mutex);
    m_ioCancellable = false;
}

void FileWorkerThread::cancelPendingIo()
{
    // m_mutex must be held by the caller
#ifdef Q_OS_WIN
    if (m_ioCancellable && m_threadHandle)
        CancelSynchronousIo(static_cast<HANDLE>(m_threadHandle));   // read/write fails with ERROR_OPERATION_ABORTED
#else
    // TODO(Linux): CancelSynchronousIo() is Windows API — no equivalent here yet.
    // A read on a dead CIFS / NFS mount blocks until the mount's timeout. Options:
    // pthread_kill() with a no-op signal handler (read returns EINTR — only on
    // interruptible mounts), or soft mounts with a short timeout.
#endif
}
