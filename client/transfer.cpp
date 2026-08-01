#include "transfer.h"
#include "connection.h"
#include "../common/utils.h"

#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QTimer>
#include <QDateTime>
#include <memory>

using FTP::Command;
using FTP::Packet;

// 单个数据通道连接的生命周期:认领 token 后发送/接收 TRANSFER_DATA,直到
// TRANSFER_DONE。不依赖 TransferQueue 的内部状态,只通过信号汇报结果。
// 注意:不能放进匿名 namespace——transfer.h 里 `class ClientTransferWorker;`
// 是在全局命名空间的前向声明,匿名 namespace 会产生一个不同的类型,导致
// TransferQueue 头文件里 QMap<QString, ClientTransferWorker*> 类型不匹配。
class ClientTransferWorker : public QObject {
    Q_OBJECT

public:
    ClientTransferWorker(const QString& host, quint16 port, const QString& token,
                          TransferTask::Direction direction, const QString& localPath,
                          qint64 resumeOffset, QObject* parent = nullptr)
        : QObject(parent)
        , m_socket(new QTcpSocket(this))
        , m_direction(direction)
        , m_localPath(localPath)
        , m_resumeOffset(resumeOffset)
        , m_token(token)
    {
        connect(m_socket, &QTcpSocket::connected, this, &ClientTransferWorker::onConnected);
        connect(m_socket, &QTcpSocket::readyRead, this, &ClientTransferWorker::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &ClientTransferWorker::onDisconnected);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &ClientTransferWorker::onBytesWritten);
        m_socket->connectToHost(host, port);
    }

    // hardCancel=false 表示暂停(本地部分文件保留,之后可续传);
    // hardCancel=true 表示彻底取消(同样保留部分文件,但不会自动重试)
    void cancel(bool hardCancel) {
        if (m_finished) return;
        m_cancelled = true;
        m_hardCancel = hardCancel;
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->abort();
        }
    }

signals:
    void progress(qint64 bytesTransferred);
    void finished(TransferOutcome outcome, const QString& message);

private slots:
    void onConnected() {
        Packet authPacket(Command::DATA_CHANNEL_AUTH);
        QDataStream out(&authPacket.payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << m_token;
        FTP::writeFramedPacket(m_socket, authPacket);

        if (m_direction == TransferTask::Direction::Upload) {
            m_file = std::make_unique<QFile>(m_localPath);
            if (!m_file->open(QIODevice::ReadOnly) || (m_resumeOffset > 0 && !m_file->seek(m_resumeOffset))) {
                finishWith(TransferOutcome::Error, QStringLiteral("无法打开本地文件"));
                return;
            }
            sendNextChunk();
        } else {
            QDir().mkpath(QFileInfo(m_localPath).absolutePath());
            m_file = std::make_unique<QFile>(m_localPath);
            const QIODevice::OpenMode mode = (m_resumeOffset > 0)
                                                  ? QIODevice::ReadWrite
                                                  : (QIODevice::WriteOnly | QIODevice::Truncate);
            if (!m_file->open(mode) || (m_resumeOffset > 0 && !m_file->seek(m_resumeOffset))) {
                finishWith(TransferOutcome::Error, QStringLiteral("无法创建本地文件"));
                return;
            }
        }
    }

    void onReadyRead() {
        m_framer.feed(m_socket->readAll());
        while (m_framer.hasPacket()) {
            handlePacket(m_framer.takePacket());
        }
    }

    void onDisconnected() {
        if (m_finished) return;
        if (m_cancelled) {
            finishWith(m_hardCancel ? TransferOutcome::Cancelled : TransferOutcome::Paused, QString());
        } else {
            finishWith(TransferOutcome::Error, QStringLiteral("连接断开"));
        }
    }

    void onBytesWritten(qint64) {
        if (m_direction == TransferTask::Direction::Upload && m_file && m_file->isOpen() && m_socket->bytesToWrite() == 0) {
            sendNextChunk();
        }
    }

private:
    void handlePacket(const Packet& packet) {
        switch (packet.command) {
        case Command::TRANSFER_DATA: {
            if (m_direction != TransferTask::Direction::Download) return;
            quint64 offset = 0;
            QByteArray data;
            if (!Packet::parseDataPacket(packet.payload, offset, data)) return;
            m_file->seek(static_cast<qint64>(offset));
            m_file->write(data);
            m_bytesThisSession = static_cast<qint64>(offset) + data.size() - m_resumeOffset;
            emit progress(m_resumeOffset + m_bytesThisSession);
            break;
        }
        case Command::TRANSFER_DONE: {
            if (m_direction != TransferTask::Direction::Download) return;
            m_file->flush();
            m_file->close();
            const QByteArray actualHash = FTP::Utils::fileHash(m_localPath);
            if (actualHash == packet.payload) {
                finishWith(TransferOutcome::Success, QString());
            } else {
                QFile::remove(m_localPath); // 校验失败,强制下次从头重传
                finishWith(TransferOutcome::Error, QStringLiteral("校验失败"));
            }
            m_socket->disconnectFromHost();
            break;
        }
        case Command::SUCCESS:
            if (m_direction == TransferTask::Direction::Upload) {
                finishWith(TransferOutcome::Success, QString());
                m_socket->disconnectFromHost();
            }
            break;
        case Command::TRANSFER_ERROR:
            if (m_direction == TransferTask::Direction::Upload) {
                finishWith(TransferOutcome::Error, QString::fromUtf8(packet.payload));
                m_socket->disconnectFromHost();
            }
            break;
        default:
            break;
        }
    }

    void sendNextChunk() {
        if (!m_file) return;
        const QByteArray chunk = m_file->read(FTP::BLOCK_SIZE);
        if (chunk.isEmpty() && m_file->atEnd()) {
            m_file->close();
            m_file.reset(); // 防止 TRANSFER_DONE flush 后残留的 bytesWritten 信号重新调用 sendNextChunk() 读已关闭的文件
            const QByteArray hash = FTP::Utils::fileHash(m_localPath);
            Packet donePacket(Command::TRANSFER_DONE);
            donePacket.payload = hash;
            FTP::writeFramedPacket(m_socket, donePacket);
            // 等待服务端 SUCCESS/TRANSFER_ERROR 回执,见 handlePacket
            return;
        }
        const qint64 chunkOffset = m_resumeOffset + m_bytesThisSession;
        m_bytesThisSession += chunk.size();
        Packet dataPacket(Command::TRANSFER_DATA);
        dataPacket.payload = Packet::createDataPacket(static_cast<quint64>(chunkOffset), chunk);
        FTP::writeFramedPacket(m_socket, dataPacket);
        emit progress(chunkOffset + chunk.size());
    }

    void finishWith(TransferOutcome outcome, const QString& message) {
        if (m_finished) return;
        m_finished = true;
        if (m_file && m_file->isOpen()) m_file->close();
        emit finished(outcome, message);
    }

    QTcpSocket* m_socket;
    FTP::PacketFramer m_framer;
    TransferTask::Direction m_direction;
    QString m_localPath;
    qint64 m_resumeOffset;
    QString m_token;

    std::unique_ptr<QFile> m_file;
    qint64 m_bytesThisSession = 0;
    bool m_cancelled = false;
    bool m_hardCancel = false;
    bool m_finished = false;
};

TransferQueue::TransferQueue(Connection* connection, QObject* parent)
    : QAbstractTableModel(parent)
    , m_connection(connection)
{
    connect(m_connection, &Connection::transferReady, this, &TransferQueue::onTransferReady);
}

void TransferQueue::enqueueUpload(const QString& localPath, const QString& remoteDir) {
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) return;

    TransferTask task;
    task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.direction = TransferTask::Direction::Upload;
    task.localPath = localPath;
    task.remotePath = (remoteDir == "/" ? "/" : remoteDir + "/") + info.fileName();
    task.totalSize = info.size();

    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    m_tasks.append(task);
    endInsertRows();
    startNextIfPossible();
}

void TransferQueue::enqueueDownload(const QString& remotePath, const QString& localDir, qint64 remoteSize) {
    TransferTask task;
    task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.direction = TransferTask::Direction::Download;
    task.remotePath = remotePath;
    const QString fileName = remotePath.section('/', -1);
    task.localPath = QDir(localDir).filePath(fileName);
    task.totalSize = remoteSize;

    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    m_tasks.append(task);
    endInsertRows();
    startNextIfPossible();
}

void TransferQueue::startNextIfPossible() {
    if (!m_pendingRequestTaskId.isEmpty()) return;
    if (m_activeCount >= m_maxConcurrent) return;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].state == TransferTask::State::Queued) {
            startTask(i);
            return;
        }
    }
}

void TransferQueue::startTask(int row) {
    TransferTask& task = m_tasks[row];
    task.state = TransferTask::State::Requesting;
    m_pendingRequestTaskId = task.id;
    updateRow(row);

    if (task.direction == TransferTask::Direction::Upload) {
        m_connection->requestUpload(task.remotePath, task.totalSize);
    } else {
        const QFileInfo localInfo(task.localPath);
        const qint64 localExisting = localInfo.exists() ? localInfo.size() : 0;
        m_connection->requestDownload(task.remotePath, localExisting);
    }
}

void TransferQueue::onTransferReady(bool success, const QString& message, const QString& token, qint64 resumeOffset) {
    const int row = rowForId(m_pendingRequestTaskId);
    m_pendingRequestTaskId.clear();
    if (row < 0) {
        startNextIfPossible();
        return;
    }

    TransferTask& task = m_tasks[row];
    if (!success) {
        task.state = TransferTask::State::Failed;
        task.statusMessage = message;
        updateRow(row);
        startNextIfPossible();
        return;
    }

    task.bytesTransferred = resumeOffset;
    task.lastSpeedSampleBytes = resumeOffset;
    task.lastSpeedSampleMsec = QDateTime::currentMSecsSinceEpoch();
    task.state = TransferTask::State::Transferring;
    updateRow(row);
    ++m_activeCount;

    auto* worker = new ClientTransferWorker(m_connection->host(), m_connection->dataPort(), token,
                                             task.direction, task.localPath, resumeOffset, this);
    m_workers.insert(task.id, worker);
    const QString taskId = task.id;
    connect(worker, &ClientTransferWorker::progress, this, [this, taskId](qint64 bytesTransferred) {
        const int r = rowForId(taskId);
        if (r >= 0) onWorkerProgress(r, bytesTransferred);
    });
    connect(worker, &ClientTransferWorker::finished, this, [this, taskId](TransferOutcome outcome, const QString& msg) {
        const int r = rowForId(taskId);
        if (r >= 0) onWorkerFinished(r, outcome, msg);
    });

    startNextIfPossible();
}

void TransferQueue::onWorkerProgress(int row, qint64 bytesTransferred) {
    TransferTask& task = m_tasks[row];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (task.lastSpeedSampleMsec == 0) {
        task.lastSpeedSampleMsec = now;
        task.lastSpeedSampleBytes = task.bytesTransferred;
    }
    const qint64 elapsedMs = now - task.lastSpeedSampleMsec;
    if (elapsedMs >= 500) {
        const qint64 deltaBytes = bytesTransferred - task.lastSpeedSampleBytes;
        task.bytesPerSecond = elapsedMs > 0 ? (deltaBytes * 1000 / elapsedMs) : 0;
        task.lastSpeedSampleMsec = now;
        task.lastSpeedSampleBytes = bytesTransferred;
    }
    task.bytesTransferred = bytesTransferred;
    updateRow(row);
}

void TransferQueue::onWorkerFinished(int row, TransferOutcome outcome, const QString& message) {
    TransferTask& task = m_tasks[row];
    if (ClientTransferWorker* worker = m_workers.take(task.id)) {
        worker->deleteLater();
    }
    --m_activeCount;

    switch (outcome) {
    case TransferOutcome::Success:
        task.state = TransferTask::State::Completed;
        task.bytesTransferred = task.totalSize;
        task.statusMessage.clear();
        task.retryCount = 0;
        break;
    case TransferOutcome::Paused:
        task.state = TransferTask::State::Paused;
        task.statusMessage = QStringLiteral("已暂停");
        break;
    case TransferOutcome::Cancelled:
        task.state = TransferTask::State::Cancelled;
        task.statusMessage = QStringLiteral("已取消");
        break;
    case TransferOutcome::Error:
        ++task.retryCount;
        task.state = TransferTask::State::Failed;
        if (task.retryCount <= 3) {
            task.statusMessage = QStringLiteral("%1(将自动重试,第 %2 次)").arg(message).arg(task.retryCount);
            scheduleRetry(row);
        } else {
            task.statusMessage = message;
        }
        break;
    }

    updateRow(row);
    startNextIfPossible();
}

void TransferQueue::scheduleRetry(int row) {
    const QString taskId = m_tasks[row].id;
    const int delayMs = qMin(m_tasks[row].retryCount, 5) * 2000;
    QTimer::singleShot(delayMs, this, [this, taskId] {
        const int r = rowForId(taskId);
        if (r < 0) return;
        if (m_tasks[r].state != TransferTask::State::Failed) return; // 用户可能已手动取消/移除
        m_tasks[r].state = TransferTask::State::Queued;
        updateRow(r);
        startNextIfPossible();
    });
}

void TransferQueue::pauseRow(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    TransferTask& task = m_tasks[row];
    if (ClientTransferWorker* worker = m_workers.value(task.id)) {
        worker->cancel(false);
    } else if (task.state == TransferTask::State::Queued) {
        task.state = TransferTask::State::Paused;
        task.statusMessage = QStringLiteral("已暂停");
        updateRow(row);
    }
}

void TransferQueue::resumeRow(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    TransferTask& task = m_tasks[row];
    if (task.state == TransferTask::State::Paused || task.state == TransferTask::State::Failed ||
        task.state == TransferTask::State::Cancelled) {
        task.state = TransferTask::State::Queued;
        task.retryCount = 0;
        task.statusMessage.clear();
        updateRow(row);
        startNextIfPossible();
    }
}

void TransferQueue::cancelRow(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    TransferTask& task = m_tasks[row];
    if (ClientTransferWorker* worker = m_workers.value(task.id)) {
        worker->cancel(true);
    } else if (task.state == TransferTask::State::Queued || task.state == TransferTask::State::Paused ||
               task.state == TransferTask::State::Failed) {
        task.state = TransferTask::State::Cancelled;
        task.statusMessage = QStringLiteral("已取消");
        updateRow(row);
    }
}

void TransferQueue::removeRow(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    const QString id = m_tasks[row].id;
    if (ClientTransferWorker* worker = m_workers.take(id)) {
        worker->disconnect(this);
        worker->cancel(true);
        worker->deleteLater();
        --m_activeCount;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_tasks.removeAt(row);
    endRemoveRows();
    startNextIfPossible();
}

void TransferQueue::clearFinished() {
    for (int i = m_tasks.size() - 1; i >= 0; --i) {
        const TransferTask::State state = m_tasks[i].state;
        if (state == TransferTask::State::Completed || state == TransferTask::State::Cancelled) {
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
    case ColProgress:
        return task.totalSize > 0 ? QStringLiteral("%1%").arg(task.bytesTransferred * 100 / task.totalSize)
                                   : QStringLiteral("--");
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
    case ColProgress: return QStringLiteral("进度");
    case ColSpeed: return QStringLiteral("速度");
    case ColEta: return QStringLiteral("剩余时间");
    case ColStatus: return QStringLiteral("状态");
    default: return {};
    }
}

#include "transfer.moc"
