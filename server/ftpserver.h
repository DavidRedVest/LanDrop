#pragma once

#include <QTcpServer>
#include <QMap>
#include <QMutex>
#include <QString>
#include <memory>

class Session;
class FileManager;
class DataChannelServer;

class FTPServer : public QTcpServer {
    Q_OBJECT

public:
    explicit FTPServer(QObject* parent = nullptr);
    ~FTPServer();

    // 控制通道监听 port,数据通道监听 port+1(参见 FTP::DATA_PORT 的默认约定)
    bool start(quint16 port = 2121);
    void stop();
    
    bool isRunning() const { return m_running; }
    quint16 serverPort() const { return m_port; }
    
    QString rootPath() const { return m_rootPath; }
    void setRootPath(const QString& path);
    
    // 用户认证配置(密码会被哈希后存储,内存中不保留明文)
    void addUser(const QString& username, const QString& password);
    void removeUser(const QString& username);
    bool authenticate(const QString& username, const QString& password) const;

    // 供 GUI 持久化用户表时使用:直接读写已哈希的密码,避免明文落盘
    QMap<QString, QString> userHashes() const;
    void addUserWithHash(const QString& username, const QString& passwordHash);

    int activeSessionCount() const;

signals:
    void clientConnected(const QString& address);
    void clientDisconnected(const QString& address);
    void logMessage(const QString& message);
    void transferStarted(const QString& sessionId, const QString& fileName, bool isUpload);
    void transferProgress(const QString& sessionId, qint64 bytesTransferred, qint64 totalBytes);
    void transferCompleted(const QString& sessionId, const QString& fileName);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void onSessionFinished();
    void onLogMessage(const QString& msg);

private:
    bool m_running = false;
    quint16 m_port = 2121;
    QString m_rootPath;
    
    QMap<QString, QString> m_users;  // username -> passwordHash
    mutable QMutex m_userMutex;
    
    QMap<QString, Session*> m_sessions;
    mutable QMutex m_sessionMutex;
    int m_sessionCounter = 0;

    std::unique_ptr<FileManager> m_fileManager;
    std::unique_ptr<DataChannelServer> m_dataChannelServer;
};
