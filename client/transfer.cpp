#include "transfer.h"
#include "../common/utils.h"
#include "../core/ftp/ftp_transfer_manager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

TransferQueue::TransferQueue(QObject* parent) : QAbstractTableModel(parent) {}

// 必须在 .cpp 里定义(即使是默认行为),即使不显式写内容——因为 m_ftpManager 是
// unique_ptr<core::FtpTransferManager>,而 transfer.h 里只前向声明了这个类型,
// 编译器生成隐式析构函数需要在"能看到完整类型定义"的地方生成,不能留给
// 只包含 transfer.h、看不到 core/ftp/ftp_transfer_manager.h 完整定义的其它 .cpp
// (比如 mainwindow.cpp)去隐式生成。
TransferQueue::~TransferQueue() = default;

void TransferQueue::setConnectionInfo(const QString& host, quint16 port, const QString& username,
                                       const QString& password) {
    if (m_ftpManager) m_ftpManager->stop(); // 重新连接/换站点:先干净停掉旧的连接池

    m_ftpManager.reset(new core::FtpTransferManager(host.toStdString(), port, username.toStdString(),
                                                      password.toStdString(), m_maxConcurrent));

    m_ftpManager->setProgressCallback([this](const std::string& id, uint64_t bytesTransferred) {
        const QString taskId = QString::fromStdString(id);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        {
            std::lock_guard<std::mutex> lock(m_progressThrottleMutex);
            auto it = m_lastProgressReportMsec.find(taskId);
            if (it != m_lastProgressReportMsec.end() && now - it->second < 200) return;
            m_lastProgressReportMsec[taskId] = now;
        }
        QMetaObject::invokeMethod(
            this, [this, taskId, bytesTransferred] { applyProgress(taskId, static_cast<qint64>(bytesTransferred)); },
            Qt::QueuedConnection);
    });

    m_ftpManager->setStateCallback(
        [this](const std::string& id, core::FtpTaskState state, const std::string& message) {
            const QString taskId = QString::fromStdString(id);
            const QString msg = QString::fromStdString(message);
            QMetaObject::invokeMethod(
                this, [this, taskId, state, msg] { applyStateChange(taskId, state, msg); }, Qt::QueuedConnection);
        });

    m_ftpManager->start();
}

void TransferQueue::enqueueUpload(const QString& localPath, const QString& remoteDir) {
    if (!m_ftpManager) return;
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) return;

    const QString remotePath = (remoteDir == "/" ? "/" : remoteDir + "/") + info.fileName();
    const std::string coreId = m_ftpManager->enqueueUpload(localPath.toStdString(), remotePath.toStdString(),
                                                             static_cast<uint64_t>(info.size()));

    TransferTask task;
    task.id = QString::fromStdString(coreId);
    task.direction = TransferTask::Direction::Upload;
    task.localPath = localPath;
    task.remotePath = remotePath;
    task.totalSize = info.size();

    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    m_tasks.append(task);
    endInsertRows();
}

void TransferQueue::enqueueDownload(const QString& remotePath, const QString& localDir, qint64 remoteSize) {
    if (!m_ftpManager) return;
    const QString fileName = remotePath.section('/', -1);
    const QString localPath = QDir(localDir).filePath(fileName);

    const std::string coreId = m_ftpManager->enqueueDownload(remotePath.toStdString(), localPath.toStdString(),
                                                               static_cast<uint64_t>(remoteSize));

    TransferTask task;
    task.id = QString::fromStdString(coreId);
    task.direction = TransferTask::Direction::Download;
    task.remotePath = remotePath;
    task.localPath = localPath;
    task.totalSize = remoteSize;

    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    m_tasks.append(task);
    endInsertRows();
}

void TransferQueue::pauseRow(int row) {
    if (row < 0 || row >= m_tasks.size() || !m_ftpManager) return;
    m_ftpManager->pauseTask(m_tasks[row].id.toStdString());
}

void TransferQueue::resumeRow(int row) {
    if (row < 0 || row >= m_tasks.size() || !m_ftpManager) return;
    m_ftpManager->resumeTask(m_tasks[row].id.toStdString());
}

void TransferQueue::cancelRow(int row) {
    if (row < 0 || row >= m_tasks.size() || !m_ftpManager) return;
    m_ftpManager->cancelTask(m_tasks[row].id.toStdString());
}

void TransferQueue::removeRow(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    if (m_ftpManager) m_ftpManager->removeTask(m_tasks[row].id.toStdString());
    beginRemoveRows(QModelIndex(), row, row);
    m_tasks.removeAt(row);
    endRemoveRows();
}

void TransferQueue::clearFinished() {
    for (int i = m_tasks.size() - 1; i >= 0; --i) {
        const TransferTask::State state = m_tasks[i].state;
        if (state == TransferTask::State::Completed || state == TransferTask::State::Cancelled) {
            if (m_ftpManager) m_ftpManager->removeTask(m_tasks[i].id.toStdString());
            beginRemoveRows(QModelIndex(), i, i);
            m_tasks.removeAt(i);
            endRemoveRows();
        }
    }
}

int TransferQueue::rowForId(const QString& id) const {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == id) return i;
    }
    return -1;
}

void TransferQueue::updateRow(int row) {
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void TransferQueue::applyProgress(const QString& id, qint64 bytesTransferred) {
    const int row = rowForId(id);
    if (row < 0) return;
    TransferTask& task = m_tasks[row];
    task.bytesTransferred = bytesTransferred;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (task.lastSpeedSampleMsec == 0) {
        task.lastSpeedSampleMsec = now;
        task.lastSpeedSampleBytes = bytesTransferred;
        updateRow(row);
        return;
    }
    const qint64 elapsedMs = now - task.lastSpeedSampleMsec;
    if (elapsedMs <= 0) {
        updateRow(row);
        return;
    }
    const qint64 deltaBytes = bytesTransferred - task.lastSpeedSampleBytes;
    task.bytesPerSecond = deltaBytes * 1000 / elapsedMs;
    task.lastSpeedSampleMsec = now;
    task.lastSpeedSampleBytes = bytesTransferred;
    updateRow(row);
}

void TransferQueue::applyStateChange(const QString& id, core::FtpTaskState state, const QString& message) {
    const int row = rowForId(id);
    if (row < 0) return;
    TransferTask& task = m_tasks[row];

    switch (state) {
    case core::FtpTaskState::Queued:
        task.state = TransferTask::State::Queued;
        task.statusMessage.clear();
        break;
    case core::FtpTaskState::Connecting:
        task.state = TransferTask::State::Requesting;
        break;
    case core::FtpTaskState::Transferring:
        task.state = TransferTask::State::Transferring;
        break;
    case core::FtpTaskState::Completed:
        task.state = TransferTask::State::Completed;
        task.bytesTransferred = task.totalSize;
        task.statusMessage.clear();
        break;
    case core::FtpTaskState::Failed:
        task.state = TransferTask::State::Failed;
        task.statusMessage = message;
        break;
    case core::FtpTaskState::Paused:
        task.state = TransferTask::State::Paused;
        task.statusMessage = QStringLiteral("已暂停");
        break;
    case core::FtpTaskState::Cancelled:
        task.state = TransferTask::State::Cancelled;
        task.statusMessage = QStringLiteral("已取消");
        break;
    }
    updateRow(row);
}

int TransferQueue::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_tasks.size();
}

int TransferQueue::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TransferQueue::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_tasks.size()) return {};
    const TransferTask& task = m_tasks.at(index.row());

    if (role == BytesTransferredRole) return task.bytesTransferred;
    if (role == TotalSizeRole) return task.totalSize;
    if (role == StateRole) return static_cast<int>(task.state);

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case ColDirection:
        return task.direction == TransferTask::Direction::Upload ? QStringLiteral("上传") : QStringLiteral("下载");
    case ColName:
        return QFileInfo(task.direction == TransferTask::Direction::Upload ? task.localPath : task.remotePath).fileName();
    case ColPercent:
        return task.totalSize > 0 ? QStringLiteral("%1%").arg(task.bytesTransferred * 100 / task.totalSize)
                                   : QStringLiteral("--");
    case ColProgress:
        return {}; // 纯视觉进度条,由 TransferWidget 用 QProgressBar 控件覆盖渲染,不需要文字
    case ColSpeed:
        return task.bytesPerSecond > 0 ? FTP::Utils::formatSpeed(task.bytesPerSecond) : QString();
    case ColEta: {
        if (task.bytesPerSecond <= 0 || task.totalSize <= 0) return QString();
        const qint64 remaining = task.totalSize - task.bytesTransferred;
        if (remaining <= 0) return QString();
        return FTP::Utils::formatDuration(static_cast<int>(remaining / task.bytesPerSecond));
    }
    case ColStatus:
        switch (task.state) {
        case TransferTask::State::Queued: return QStringLiteral("排队中");
        case TransferTask::State::Requesting: return QStringLiteral("连接中");
        case TransferTask::State::Transferring: return QStringLiteral("传输中");
        case TransferTask::State::Completed: return QStringLiteral("已完成");
        case TransferTask::State::Failed: return task.statusMessage.isEmpty() ? QStringLiteral("失败") : task.statusMessage;
        case TransferTask::State::Paused: return QStringLiteral("已暂停");
        case TransferTask::State::Cancelled: return QStringLiteral("已取消");
        }
        return {};
    default:
        return {};
    }
}

QVariant TransferQueue::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case ColDirection: return QStringLiteral("方向");
    case ColName: return QStringLiteral("文件");
    case ColPercent: return QStringLiteral("百分比");
    case ColProgress: return QStringLiteral("进度");
    case ColSpeed: return QStringLiteral("速度");
    case ColEta: return QStringLiteral("剩余时间");
    case ColStatus: return QStringLiteral("状态");
    default: return {};
    }
}
