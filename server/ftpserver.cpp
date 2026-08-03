#include "ftpserver.h"

#include "../common/utils.h"
#include "../core/discovery/lan_discovery.h"
#include "../core/ftp/ftp_server.h"

#include <QDir>
#include <QMetaObject>

FTPServer::FTPServer(QObject* parent) : QObject(parent), m_core(new core::FtpServer()) {
    m_rootPath = QDir::homePath() + "/LanDrop_Server";
    QDir().mkpath(m_rootPath);
    m_core->setRootPath(m_rootPath.toStdString());

    m_core->setAuthenticator(
        [this](const std::string& user, const std::string& pass) {
            return authenticate(QString::fromStdString(user), QString::fromStdString(pass));
        });

    m_core->setLogCallback([this](const std::string& peerAddress, const std::string& message) {
        const QString text = QStringLiteral("%1: %2").arg(QString::fromStdString(peerAddress),
                                                            QString::fromStdString(message));
        QMetaObject::invokeMethod(this, [this, text] { emit logMessage(text); }, Qt::QueuedConnection);
    });
    m_core->setTransferStartedCallback(
        [this](const std::string& sessionId, const std::string& fileName, bool isUpload) {
            const QString id = QString::fromStdString(sessionId);
            const QString name = QString::fromStdString(fileName);
            QMetaObject::invokeMethod(
                this, [this, id, name, isUpload] { emit transferStarted(id, name, isUpload); }, Qt::QueuedConnection);
        });
    m_core->setTransferProgressCallback(
        [this](const std::string& sessionId, uint64_t transferred, uint64_t total) {
            const QString id = QString::fromStdString(sessionId);
            const qint64 t = static_cast<qint64>(transferred);
            const qint64 tot = static_cast<qint64>(total);
            QMetaObject::invokeMethod(
                this, [this, id, t, tot] { emit transferProgress(id, t, tot); }, Qt::QueuedConnection);
        });
    m_core->setTransferCompletedCallback([this](const std::string& sessionId, const std::string& fileName) {
        const QString id = QString::fromStdString(sessionId);
        const QString name = QString::fromStdString(fileName);
        QMetaObject::invokeMethod(this, [this, id, name] { emit transferCompleted(id, name); }, Qt::QueuedConnection);
    });
}

FTPServer::~FTPServer() {
    stop();
}

bool FTPServer::start(quint16 port) {
    if (m_running) return true;

    if (!m_core->start(port)) {
        emit logMessage(QStringLiteral("控制端口 %1 监听失败").arg(port));
        return false;
    }

    m_port = port;
    m_running = true;
    emit logMessage(QStringLiteral("服务已启动: 端口 %1").arg(port));
    emit logMessage(QStringLiteral("根目录: %1").arg(m_rootPath));
    return true;
}

void FTPServer::stop() {
    if (!m_running) return;

    m_discoveryBeacon.reset(); // 服务停止时局域网广播也一并停止
    m_core->stop();
    m_running = false;
    emit logMessage(QStringLiteral("服务已停止"));
}

void FTPServer::setRootPath(const QString& path) {
    m_rootPath = path;
    QDir().mkpath(m_rootPath);
    m_core->setRootPath(m_rootPath.toStdString());
}

void FTPServer::addUser(const QString& username, const QString& password) {
    std::lock_guard<std::mutex> lock(m_usersMutex);
    m_users[username] = FTP::Utils::hashPassword(password);
}

void FTPServer::removeUser(const QString& username) {
    std::lock_guard<std::mutex> lock(m_usersMutex);
    m_users.remove(username);
}

bool FTPServer::authenticate(const QString& username, const QString& password) const {
    std::lock_guard<std::mutex> lock(m_usersMutex);
    const auto it = m_users.find(username);
    if (it == m_users.end()) return false;
    return FTP::Utils::verifyPassword(password, it.value());
}

QMap<QString, QString> FTPServer::userHashes() const {
    std::lock_guard<std::mutex> lock(m_usersMutex);
    return m_users;
}

void FTPServer::addUserWithHash(const QString& username, const QString& passwordHash) {
    std::lock_guard<std::mutex> lock(m_usersMutex);
    m_users[username] = passwordHash;
}

int FTPServer::activeSessionCount() const {
    return m_core->activeSessionCount();
}

void FTPServer::setDiscoveryEnabled(bool enabled, const QString& deviceName) {
    if (!enabled) {
        m_discoveryBeacon.reset();
        return;
    }
    if (!m_running) return; // 没有端口可广播,忽略
    m_discoveryBeacon.reset(new core::DiscoveryBeacon(deviceName.toStdString(), m_port));
    m_discoveryBeacon->start();
}

bool FTPServer::isDiscoveryEnabled() const {
    return m_discoveryBeacon && m_discoveryBeacon->isRunning();
}
