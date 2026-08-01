#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QAbstractSocket>

#include "../common/protocol.h"

class QTcpSocket;
class QTimer;

// 客户端控制通道:登录、浏览、文件管理、发起传输请求。
// 所有操作都是异步的,结果通过信号通知;真正的文件字节收发由
// TransferQueue 在收到 transferReady 后另开一条数据连接完成。
class Connection : public QObject {
    Q_OBJECT

public:
    explicit Connection(QObject* parent = nullptr);

    void connectToHost(const QString& host, quint16 port = FTP::DEFAULT_PORT);
    void disconnectFromHost();
    bool isConnected() const;

    QString host() const { return m_host; }
    quint16 controlPort() const { return m_port; }
    quint16 dataPort() const { return static_cast<quint16>(m_port + 1); }

    void login(const QString& username, const QString& password);
    void listDirectory(const QString& path);
    void mkdir(const QString& path);
    void rmdir(const QString& path);
    void deleteFile(const QString& path);
    void rename(const QString& oldPath, const QString& newPath);
    void changeDir(const QString& path);

    void requestUpload(const QString& path, qint64 fileSize);
    void requestDownload(const QString& path, qint64 localExistingSize);
    void cancelTransfer(const QString& token);

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void loginResult(bool success, const QString& message);
    void directoryListed(bool success, const QString& path, const QString& message, const QList<FTP::FileInfo>& list);
    void operationResult(bool success, const QString& message);
    void directoryChanged(bool success, const QString& canonicalPath, const QString& message);
    void transferReady(bool success, const QString& message, const QString& token, qint64 resumeOffset);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void onHeartbeatTimeout();

private:
    void handlePacket(const FTP::Packet& packet);
    void sendPacket(const FTP::Packet& packet);

    QTcpSocket* m_socket;
    FTP::PacketFramer m_framer;
    QString m_host;
    quint16 m_port = FTP::DEFAULT_PORT;
    QTimer* m_heartbeatTimer;
    bool m_pongPending = false;
};
