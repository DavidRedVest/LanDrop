#include "foldertransfer.h"
#include "connection.h"
#include "transfer.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

FolderTransferCoordinator::FolderTransferCoordinator(Connection* connection, TransferQueue* transferQueue,
                                                       QObject* parent)
    : QObject(parent), m_connection(connection), m_transferQueue(transferQueue) {
    connect(m_connection, &Connection::operationResult, this, &FolderTransferCoordinator::onOperationResult);
    connect(m_connection, &Connection::directoryListed, this, &FolderTransferCoordinator::onDirectoryListed);
}

void FolderTransferCoordinator::uploadFolders(const QStringList& localFolderPaths, const QString& remoteBaseDir) {
    for (const QString& folderPath : localFolderPaths) {
        const QFileInfo folderInfo(folderPath);
        const QString topRemoteDir = (remoteBaseDir == "/" ? "/" : remoteBaseDir + "/") + folderInfo.fileName();

        PendingFolderUpload job;
        job.remoteDirsToCreate.append(topRemoteDir);

        // Subdirectories 是深度优先遍历,一个目录本身总是在它自己的子项之前被
        // 迭代到——这正好给了我们需要的"父目录在前"的建目录顺序,不用额外排序。
        QDirIterator it(folderPath, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo entryInfo = it.fileInfo();
            const QString relative = QDir(folderPath).relativeFilePath(entryInfo.absoluteFilePath());
            if (entryInfo.isDir()) {
                job.remoteDirsToCreate.append(topRemoteDir + "/" + relative);
            } else {
                const QString relDir = QFileInfo(relative).path();
                const QString remoteDir = (relDir == ".") ? topRemoteDir : topRemoteDir + "/" + relDir;
                job.files.append({entryInfo.absoluteFilePath(), remoteDir});
            }
        }
        m_pendingUploads.enqueue(job);
    }
    startNextUploadIfIdle();
}

void FolderTransferCoordinator::startNextUploadIfIdle() {
    if (m_uploadInFlight || m_pendingUploads.isEmpty()) return;
    const PendingFolderUpload job = m_pendingUploads.dequeue();
    if (job.remoteDirsToCreate.isEmpty()) return; // 不会发生,防御一下

    m_uploadInFlight = true;
    m_uploadMkdirRemaining = job.remoteDirsToCreate.size();
    m_uploadPendingFiles = job.files;
    for (const QString& dir : job.remoteDirsToCreate) m_connection->mkdir(dir);
}

void FolderTransferCoordinator::onOperationResult(bool /*success*/, const QString& /*message*/) {
    if (!m_uploadInFlight) return; // 不是文件夹上传触发的 mkdir,和我们无关
    if (--m_uploadMkdirRemaining > 0) return;

    for (const auto& file : m_uploadPendingFiles) m_transferQueue->enqueueUpload(file.first, file.second);
    m_uploadPendingFiles.clear();
    m_uploadInFlight = false;
    startNextUploadIfIdle();
}

void FolderTransferCoordinator::downloadFolders(const QStringList& remoteFolderPaths, const QString& localBaseDir) {
    for (const QString& remoteFolderPath : remoteFolderPaths) {
        const QString folderName = remoteFolderPath.section('/', -1);
        const QString localDir = QDir(localBaseDir).filePath(folderName);
        QDir().mkpath(localDir);
        m_downloadQueue.enqueue({remoteFolderPath, localDir});
    }
    startNextDownloadStepIfIdle();
}

void FolderTransferCoordinator::startNextDownloadStepIfIdle() {
    if (m_downloadInFlight || m_downloadQueue.isEmpty()) return;
    const auto next = m_downloadQueue.dequeue();
    m_downloadCurrentRemote = next.first;
    m_downloadCurrentLocal = next.second;
    m_downloadInFlight = true;
    m_connection->listDirectory(next.first);
}

void FolderTransferCoordinator::onDirectoryListed(bool success, const QString& path, const QString& /*message*/,
                                                    const QList<FTP::FileInfo>& list) {
    // path 和我们当前正等待的那次 LIST 请求的路径不一致,说明这是别处(比如
    // FileBrowserPanel 自己浏览)触发的、和这次文件夹下载无关的响应,忽略。
    if (!m_downloadInFlight || path != m_downloadCurrentRemote) return;
    m_downloadInFlight = false;

    if (success) {
        for (const FTP::FileInfo& info : list) {
            const QString childRemote =
                (m_downloadCurrentRemote == "/" ? "/" : m_downloadCurrentRemote + "/") + info.name;
            if (info.isDirectory) {
                const QString childLocal = QDir(m_downloadCurrentLocal).filePath(info.name);
                QDir().mkpath(childLocal);
                m_downloadQueue.enqueue({childRemote, childLocal});
            } else {
                m_transferQueue->enqueueDownload(childRemote, m_downloadCurrentLocal, info.size);
            }
        }
    }
    startNextDownloadStepIfIdle();
}
