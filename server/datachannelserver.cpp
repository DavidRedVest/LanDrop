#include "datachannelserver.h"
#include "filemanager.h"
#include "../common/protocol.h"
#include "../common/utils.h"

#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

using FTP::Command;
using FTP::Packet;

// 单个数据通道连接的生命周期处理:认领 token、收发字节、完成后自毁。
// 注意:不能放进匿名 namespace——匿名 namespace 会给它一个仅在本 TU 内可见的
// 链接名,与 DataChannelServer 头文件里 `friend class DataTransferWorker;`
// (在全局命名空间查找/声明)不是同一个类型,friend 授权会失效导致私有成员访问失败。
class DataTransferWorker : public QObject {
    Q_OBJECT

public:
    DataTransferWorker(qintptr socketDescriptor, DataChannelServer* server, FileManager* fileManager)
        : m_socket(new QTcpSocket(this))
        , m_server(server)
        , m_fileManager(fileManager)
    {
        m_socket->setSocketDescriptor(socketDescriptor);
        connect(m_socket, &QTcpSocket::readyRead, this, &DataTransferWorker::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &DataTransferWorker::onDisconnected);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &DataTransferWorker::onBytesWritten);
    }

private slots:
    void onReadyRead() {
        m_framer.feed(m_socket->readAll());
        while (m_framer.hasPacket()) {
            handlePacket(m_framer.takePacket());
        }
    }

    void onDisconnected() {
        // disconnectFromHost() 是异步的:调用后它需要先把已排队的数据(比如
        // TRANSFER_DONE 的 SUCCESS 回执)flush 完才会真正断开。真正的收尾
        // (finishTransfer + deleteLater)必须等这个信号发生才做,否则在
        // "写完响应就立刻 deleteLater 自己"的旧写法下,socket 是 this 的子
        // 对象,提前销毁会把还没发出去的数据一起丢掉,导致客户端只看到
        // "远端关闭连接"而收不到本该到达的响应。
        if (m_file) {
            m_file->close();
        }
        if (!m_closeExpected) {
            emit m_server->transferFailed(m_token, QStringLiteral("连接已断开"));
        }
        finishTransfer();
        deleteLater();
    }

    void onBytesWritten(qint64) {
        if (m_file && m_file->isOpen() && m_socket->bytesToWrite() == 0) {
            sendNextChunk();
        }
    }

private:
    void handlePacket(const Packet& packet) {
        if (!m_authenticated) {
            if (packet.command != Command::DATA_CHANNEL_AUTH) {
                sendErrorAndClose(QStringLiteral("需要先进行 DATA_CHANNEL_AUTH"));
                return;
            }
            authenticate(packet.payload);
            return;
        }

        switch (packet.command) {
        case Command::TRANSFER_DATA:
            handleTransferData(packet);
            break;
        case Command::TRANSFER_DONE:
            handleTransferDone(packet);
            break;
        case Command::TRANSFER_CANCEL:
            if (m_file) m_file->close();
            emit m_server->transferFailed(m_token, QStringLiteral("客户端取消"));
            m_closeExpected = true;
            m_socket->disconnectFromHost();
            break;
        default:
            break;
        }
    }

    void authenticate(const QByteArray& tokenPayload) {
        QDataStream in(tokenPayload);
        in.setVersion(QDataStream::Qt_6_0);
        in >> m_token;

        DataChannelServer::PendingTransfer transfer;
        if (!m_server->takePendingTransfer(m_token, transfer)) {
            sendErrorAndClose(QStringLiteral("token 无效或已过期"));
            return;
        }
        if (!m_server->hasCapacity()) {
            sendErrorAndClose(QStringLiteral("服务器繁忙,请稍后重试"));
            return;
        }

        m_transfer = transfer;
        m_authenticated = true;
        m_server->onWorkerStarted();

        QString errorMessage;
        const QString fileName = QFileInfo(m_transfer.relativePath).fileName();
        if (m_transfer.clientIsUploading) {
            m_file = m_fileManager->openForWrite(m_transfer.relativePath, m_transfer.resumeOffset, errorMessage);
        } else {
            m_file = m_fileManager->openForRead(m_transfer.relativePath, m_transfer.resumeOffset, errorMessage);
        }
        if (!m_file) {
            sendErrorAndClose(errorMessage);
            return;
        }

        emit m_server->transferStarted(m_token, fileName, m_transfer.clientIsUploading);
        if (!m_transfer.clientIsUploading) {
            sendNextChunk();
        }
    }

    void handleTransferData(const Packet& packet) {
        if (!m_file || !m_transfer.clientIsUploading) return;
        quint64 offset = 0;
        QByteArray data;
        if (!Packet::parseDataPacket(packet.payload, offset, data)) return;

        m_file->seek(static_cast<qint64>(offset));
        m_file->write(data);
        m_bytesTransferred = static_cast<qint64>(offset) + data.size() - m_transfer.resumeOffset;
        emit m_server->transferProgress(m_token, m_transfer.resumeOffset + m_bytesTransferred, m_transfer.expectedTotalSize);
    }

    void handleTransferDone(const Packet& packet) {
        if (!m_transfer.clientIsUploading) return;
        m_file->flush();
        m_file->close();

        const QString absPath = m_fileManager->absolutePath(m_transfer.relativePath);
        const QByteArray actualHash = FTP::Utils::fileHash(absPath);
        const QString fileName = QFileInfo(m_transfer.relativePath).fileName();

        Packet response;
        if (actualHash == packet.payload) {
            response.command = Command::SUCCESS;
            emit m_server->transferCompleted(m_token, fileName);
        } else {
            QFile::remove(absPath); // 校验失败,删除损坏文件强制下次从头重传
            response.command = Command::TRANSFER_ERROR;
            response.payload = QStringLiteral("校验失败").toUtf8();
            emit m_server->transferFailed(m_token, QStringLiteral("校验失败"));
        }
        FTP::writeFramedPacket(m_socket, response);
        m_closeExpected = true;
        m_socket->disconnectFromHost();
    }

    void sendNextChunk() {
        if (!m_file) return;
        const QByteArray chunk = m_file->read(FTP::BLOCK_SIZE);
        if (chunk.isEmpty() && m_file->atEnd()) {
            m_file->close();
            m_file.reset(); // 防止 TRANSFER_DONE flush 后残留的 bytesWritten 信号重新调用 sendNextChunk() 读已关闭的文件
            const QString absPath = m_fileManager->absolutePath(m_transfer.relativePath);
            const QByteArray hash = FTP::Utils::fileHash(absPath);
            Packet donePacket(Command::TRANSFER_DONE);
            donePacket.payload = hash;
            FTP::writeFramedPacket(m_socket, donePacket);
            emit m_server->transferCompleted(m_token, QFileInfo(m_transfer.relativePath).fileName());
            m_closeExpected = true;
            m_socket->disconnectFromHost();
            return;
        }

        const qint64 chunkOffset = m_transfer.resumeOffset + m_bytesTransferred;
        m_bytesTransferred += chunk.size();
        Packet dataPacket(Command::TRANSFER_DATA);
        dataPacket.payload = Packet::createDataPacket(static_cast<quint64>(chunkOffset), chunk);
        FTP::writeFramedPacket(m_socket, dataPacket);
        emit m_server->transferProgress(m_token, chunkOffset + chunk.size(), m_transfer.expectedTotalSize);
    }

    void sendErrorAndClose(const QString& message) {
        Packet errorPacket(Command::TRANSFER_ERROR);
        errorPacket.payload = message.toUtf8();
        FTP::writeFramedPacket(m_socket, errorPacket);
        emit m_server->logMessage(QStringLiteral("数据通道错误: %1").arg(message));
        m_closeExpected = true;
        m_socket->disconnectFromHost();
    }

    void finishTransfer() {
        if (m_finished) return;
        m_finished = true;
        if (m_authenticated) {
            m_server->onWorkerFinished();
        }
    }

    QTcpSocket* m_socket;
    FTP::PacketFramer m_framer;
    DataChannelServer* m_server;
    FileManager* m_fileManager;

    bool m_authenticated = false;
    bool m_finished = false;
    bool m_closeExpected = false; // 断开是我们主动发起的(已完成/已出错/被取消),不算意外掉线
    QString m_token;
    DataChannelServer::PendingTransfer m_transfer;
    std::unique_ptr<QFile> m_file;
    qint64 m_bytesTransferred = 0;
};

DataChannelServer::DataChannelServer(FileManager* fileManager, QObject* parent)
    : QTcpServer(parent)
    , m_fileManager(fileManager)
{
}

int DataChannelServer::activeTransferCount() const {
    return m_activeCount;
}

QString DataChannelServer::registerPendingTransfer(const QString& relativePath, bool clientIsUploading,
                                                     qint64 clientReportedSize, qint64& outResumeOffset) {
    PendingTransfer pt;
    pt.relativePath = relativePath;
    pt.clientIsUploading = clientIsUploading;

    if (clientIsUploading) {
        outResumeOffset = m_fileManager->fileSize(relativePath);
        pt.expectedTotalSize = clientReportedSize;
    } else {
        const qint64 remoteSize = m_fileManager->fileSize(relativePath);
        outResumeOffset = qBound<qint64>(0, clientReportedSize, remoteSize);
        pt.expectedTotalSize = remoteSize;
    }
    pt.resumeOffset = outResumeOffset;

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pending.insert(token, pt);
    return token;
}

void DataChannelServer::cancelTransfer(const QString& token) {
    m_pending.remove(token);
}

bool DataChannelServer::takePendingTransfer(const QString& token, PendingTransfer& outTransfer) {
    const auto it = m_pending.find(token);
    if (it == m_pending.end()) {
        return false;
    }
    outTransfer = it.value();
    m_pending.erase(it);
    return true;
}

void DataChannelServer::onWorkerStarted() {
    ++m_activeCount;
}

void DataChannelServer::onWorkerFinished() {
    --m_activeCount;
}

void DataChannelServer::incomingConnection(qintptr socketDescriptor) {
    new DataTransferWorker(socketDescriptor, this, m_fileManager);
}

#include "datachannelserver.moc"
