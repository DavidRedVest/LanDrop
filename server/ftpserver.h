#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <memory>
#include <mutex>

namespace core {
class FtpServer;
class DiscoveryBeacon;
} // namespace core

// 服务端:薄 Qt 包装,内部委托给 core::FtpServer(真实 RFC 959 服务端,accept 线程 +
// 每客户端一条 std::thread,详见 core/ftp/ftp_server.h)。公开接口(方法名/信号)
// 和旧的自定义协议实现保持一致,ServerWindow 不需要改动调用方式。
//
// 不再继承 QTcpServer——core::FtpServer 自己在原生 socket 上做 accept,这个类
// 只是把它的 std::function 回调通过 QMetaObject::invokeMethod(...,
// Qt::QueuedConnection) 转成 Qt 信号。authenticate() 在旧代码里虽然是 public,
// 但确认过除了旧版 incomingConnection() 内部自己用之外没有别的调用方,这次改成
// private——core::FtpServer 的 Authenticator 回调直接用一个内部 lambda 接管。
class FTPServer : public QObject {
    Q_OBJECT

public:
    explicit FTPServer(QObject* parent = nullptr);
    ~FTPServer() override;

    bool start(quint16 port = 2121);
    void stop();
    bool isRunning() const { return m_running; }
    quint16 serverPort() const { return m_port; }

    QString rootPath() const { return m_rootPath; }
    void setRootPath(const QString& path);

    // 用户认证配置(密码会被哈希后存储,内存中不保留明文)
    void addUser(const QString& username, const QString& password);
    void removeUser(const QString& username);

    // 供 GUI 持久化用户表时使用:直接读写已哈希的密码,避免明文落盘
    QMap<QString, QString> userHashes() const;
    void addUserWithHash(const QString& username, const QString& passwordHash);

    int activeSessionCount() const;

    // 局域网自动发现(core/discovery/lan_discovery.h):enabled=true 且服务正在
    // 运行时,开始按 deviceName 周期广播;服务未运行时调用会被忽略(没有端口可广播)。
    // 服务 stop() 时会自动一并停止广播,不需要调用方单独处理。
    void setDiscoveryEnabled(bool enabled, const QString& deviceName);
    bool isDiscoveryEnabled() const;

signals:
    void clientConnected(const QString& address);
    void clientDisconnected(const QString& address);
    void logMessage(const QString& message);
    void transferStarted(const QString& sessionId, const QString& fileName, bool isUpload);
    void transferProgress(const QString& sessionId, qint64 bytesTransferred, qint64 totalBytes);
    void transferCompleted(const QString& sessionId, const QString& fileName);

private:
    bool authenticate(const QString& username, const QString& password) const;

    std::unique_ptr<core::FtpServer> m_core;
    std::unique_ptr<core::DiscoveryBeacon> m_discoveryBeacon;

    bool m_running = false;
    quint16 m_port = 2121;
    QString m_rootPath;

    QMap<QString, QString> m_users; // username -> passwordHash
    mutable std::mutex m_usersMutex; // authenticate() 会从 core::FtpServer 的会话后台线程并发调用
};
