#pragma once

#include <QObject>
#include <QString>
#include <functional>

#include "../common/protocol.h"

class QTcpSocket;
class QTimer;
class FileManager;
class DataChannelServer;

// 一个客户端的控制通道会话:登录、浏览、文件管理、发起传输。
// 实际的文件字节收发发生在 DataChannelServer 上的独立连接里。
class Session : public QObject {
    Q_OBJECT

public:
    using Authenticator = std::function<bool(const QString&, const QString&)>;

    Session(qintptr socketDescriptor, FileManager* fileManager, DataChannelServer* dataChannelServer,
            Authenticator authenticator, QObject* parent = nullptr);

    QString peerAddress() const;
    QString username() const { return m_username; }
    bool isAuthenticated() const { return !m_username.isEmpty(); }

signals:
    void logMessage(const QString& message);
    void finished();

private slots:
    void onReadyRead();
    void onDisconnected();
    void onHeartbeatTimeout();

private:
    void handlePacket(const FTP::Packet& packet);
    void sendPacket(const FTP::Packet& packet);
    void sendError(const QString& message);
    void sendSuccess(const QString& message = QString());

    void handleLogin(const FTP::Packet& packet);
    void handleListFiles(const FTP::Packet& packet);
    void handleMkdir(const FTP::Packet& packet);
    void handleRmdir(const FTP::Packet& packet);
    void handleDeleteFile(const FTP::Packet& packet);
    void handleRename(const FTP::Packet& packet);
    void handleChangeDir(const FTP::Packet& packet);
    void handleUploadStart(const FTP::Packet& packet);
    void handleDownloadStart(const FTP::Packet& packet);
    void handleTransferCancel(const FTP::Packet& packet);

    QTcpSocket* m_socket;
    FTP::PacketFramer m_framer;
    FileManager* m_fileManager;
    DataChannelServer* m_dataChannelServer;
    Authenticator m_authenticator;
    QString m_username;
    QTimer* m_heartbeatTimer;
    bool m_pongPending = false;
};
