#include "ftpserver.h"
#include "session.h"
#include "filemanager.h"
#include "datachannelserver.h"
#include "../common/utils.h"

#include <QTcpSocket>
#include <QDir>

FTPServer::FTPServer(QObject* parent) : QTcpServer(parent) {
    m_rootPath = QDir::homePath() + "/LanDrop_Server";
    QDir().mkpath(m_rootPath);

    m_fileManager = std::make_unique<FileManager>(m_rootPath);
    m_dataChannelServer = std::make_unique<DataChannelServer>(m_fileManager.get(), this);

    connect(m_dataChannelServer.get(), &DataChannelServer::logMessage, this, &FTPServer::logMessage);
    connect(m_dataChannelServer.get(), &DataChannelServer::transferStarted, this, &FTPServer::transferStarted);
    connect(m_dataChannelServer.get(), &DataChannelServer::transferProgress, this, &FTPServer::transferProgress);
    connect(m_dataChannelServer.get(), &DataChannelServer::transferCompleted, this, &FTPServer::transferCompleted);
}

FTPServer::~FTPServer() {
    stop();
}

bool FTPServer::start(quint16 port) {
    if (m_running) {
        return true;
    }

    m_port = port;
    const quint16 dataPort = static_cast<quint16>(port + 1);

    if (!listen(QHostAddress::Any, port)) {
        emit logMessage(QString("控制端口 %1 监听失败").arg(port));
        return false;
    }
    if (!m_dataChannelServer->listen(QHostAddress::Any, dataPort)) {
        close();
        emit logMessage(QString("数据端口 %1 监听失败").arg(dataPort));
        return false;
    }

    m_running = true;
    emit logMessage(QString("服务已启动: 控制端口 %1, 数据端口 %2").arg(port).arg(dataPort));
    emit logMessage(QString("根目录: %1").arg(m_rootPath));
    return true;
}

void FTPServer::stop() {
    if (!m_running) {
        return;
    }

    close();
    m_dataChannelServer->close();

    QMutexLocker locker(&m_sessionMutex);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        it.value()->deleteLater();
    }
    m_sessions.clear();

    m_running = false;
    emit logMessage("服务已停止");
}

void FTPServer::setRootPath(const QString& path) {
    m_rootPath = path;
    QDir().mkpath(m_rootPath);
    m_fileManager->setRootPath(m_rootPath);
}

void FTPServer::addUser(const QString& username, const QString& password) {
    QMutexLocker locker(&m_userMutex);
    m_users[username] = FTP::Utils::hashPassword(password);
}

void FTPServer::removeUser(const QString& username) {
    QMutexLocker locker(&m_userMutex);
    m_users.remove(username);
}

bool FTPServer::authenticate(const QString& username, const QString& password) const {
    QMutexLocker locker(&m_userMutex);
    const auto it = m_users.find(username);
    if (it == m_users.end()) {
        return false;
    }
    return FTP::Utils::verifyPassword(password, it.value());
}

QMap<QString, QString> FTPServer::userHashes() const {
    QMutexLocker locker(&m_userMutex);
    return m_users;
}

void FTPServer::addUserWithHash(const QString& username, const QString& passwordHash) {
    QMutexLocker locker(&m_userMutex);
    m_users[username] = passwordHash;
}

int FTPServer::activeSessionCount() const {
    QMutexLocker locker(&m_sessionMutex);
    return m_sessions.size();
}

void FTPServer::incomingConnection(qintptr socketDescriptor) {
    const QString sessionId = QString("session_%1").arg(++m_sessionCounter);

    auto authenticator = [this](const QString& user, const QString& pass) {
        return authenticate(user, pass);
    };

    Session* session = new Session(socketDescriptor, m_fileManager.get(), m_dataChannelServer.get(),
                                    authenticator, this);

    connect(session, &Session::finished, this, &FTPServer::onSessionFinished);
    connect(session, &Session::logMessage, this, &FTPServer::onLogMessage);

    const QString address = session->peerAddress();
    emit clientConnected(address);
    emit logMessage(QString("客户端已连接: %1 (%2)").arg(address, sessionId));

    QMutexLocker locker(&m_sessionMutex);
    m_sessions[sessionId] = session;
    session->setProperty("sessionId", sessionId);
}

void FTPServer::onSessionFinished() {
    Session* session = qobject_cast<Session*>(sender());
    if (!session) return;

    const QString sessionId = session->property("sessionId").toString();
    const QString address = session->peerAddress();

    QMutexLocker locker(&m_sessionMutex);
    m_sessions.remove(sessionId);
    locker.unlock();

    emit clientDisconnected(address);
    emit logMessage(QString("客户端已断开: %1").arg(address));

    session->deleteLater();
}

void FTPServer::onLogMessage(const QString& msg) {
    emit logMessage(msg);
}
