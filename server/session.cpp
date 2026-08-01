#include "session.h"
#include "filemanager.h"
#include "datachannelserver.h"

#include <QTcpSocket>
#include <QTimer>
#include <QFileInfo>

using FTP::Command;
using FTP::Packet;

namespace {
constexpr int kHeartbeatIntervalMs = 15000;
}

Session::Session(qintptr socketDescriptor, FileManager* fileManager, DataChannelServer* dataChannelServer,
                  Authenticator authenticator, QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_fileManager(fileManager)
    , m_dataChannelServer(dataChannelServer)
    , m_authenticator(std::move(authenticator))
{
    m_socket->setSocketDescriptor(socketDescriptor);
    connect(m_socket, &QTcpSocket::readyRead, this, &Session::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &Session::onDisconnected);

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &Session::onHeartbeatTimeout);
    m_heartbeatTimer->start(kHeartbeatIntervalMs);
}

QString Session::peerAddress() const {
    return m_socket->peerAddress().toString();
}

void Session::onReadyRead() {
    m_framer.feed(m_socket->readAll());
    while (m_framer.hasPacket()) {
        handlePacket(m_framer.takePacket());
    }
}

void Session::onDisconnected() {
    emit finished();
}

void Session::onHeartbeatTimeout() {
    if (m_pongPending) {
        emit logMessage(QStringLiteral("心跳超时,断开连接: %1").arg(peerAddress()));
        m_socket->disconnectFromHost();
        return;
    }
    sendPacket(Packet(Command::PING));
    m_pongPending = true;
}

void Session::handlePacket(const Packet& packet) {
    if (!isAuthenticated() && packet.command != Command::LOGIN && packet.command != Command::PING) {
        sendError(QStringLiteral("请先登录"));
        return;
    }

    switch (packet.command) {
    case Command::LOGIN: handleLogin(packet); break;
    case Command::LOGOUT: m_socket->disconnectFromHost(); break;
    case Command::LIST_FILES: handleListFiles(packet); break;
    case Command::MKDIR: handleMkdir(packet); break;
    case Command::RMDIR: handleRmdir(packet); break;
    case Command::DELETE_FILE: handleDeleteFile(packet); break;
    case Command::RENAME: handleRename(packet); break;
    case Command::CHANGE_DIR: handleChangeDir(packet); break;
    case Command::UPLOAD_START: handleUploadStart(packet); break;
    case Command::DOWNLOAD_START: handleDownloadStart(packet); break;
    case Command::TRANSFER_CANCEL: handleTransferCancel(packet); break;
    case Command::PING: sendPacket(Packet(Command::PONG)); break;
    case Command::PONG: m_pongPending = false; break;
    default: sendError(QStringLiteral("未知命令")); break;
    }
}

void Session::sendPacket(const Packet& packet) {
    FTP::writeFramedPacket(m_socket, packet);
}

void Session::sendError(const QString& message) {
    Packet p(Command::ERROR);
    p.payload = message.toUtf8();
    sendPacket(p);
}

void Session::sendSuccess(const QString& message) {
    Packet p(Command::SUCCESS);
    p.payload = message.toUtf8();
    sendPacket(p);
}

void Session::handleLogin(const Packet& packet) {
    QString username, password;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> username >> password;

    const bool ok = m_authenticator && m_authenticator(username, password);

    Packet resp(Command::LOGIN_RESP);
    QDataStream out(&resp.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << ok << (ok ? QStringLiteral("登录成功") : QStringLiteral("用户名或密码错误"));
    sendPacket(resp);

    if (ok) {
        m_username = username;
        emit logMessage(QStringLiteral("用户 %1 登录成功 (%2)").arg(username, peerAddress()));
    } else {
        emit logMessage(QStringLiteral("用户 %1 登录失败 (%2)").arg(username, peerAddress()));
    }
}

void Session::handleListFiles(const Packet& packet) {
    QString path;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path;

    QList<FTP::FileInfo> list;
    QString errorMessage;
    const bool ok = m_fileManager->listDirectory(path, list, errorMessage);

    Packet resp(Command::FILE_LIST);
    QDataStream out(&resp.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << ok << errorMessage << path;
    if (ok) {
        FTP::writeFileInfoList(out, list);
    }
    sendPacket(resp);
}

void Session::handleMkdir(const Packet& packet) {
    QString path;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path;

    QString errorMessage;
    if (m_fileManager->mkdir(path, errorMessage)) {
        sendSuccess();
    } else {
        sendError(errorMessage);
    }
}

void Session::handleRmdir(const Packet& packet) {
    QString path;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path;

    QString errorMessage;
    if (m_fileManager->rmdir(path, errorMessage)) {
        sendSuccess();
    } else {
        sendError(errorMessage);
    }
}

void Session::handleDeleteFile(const Packet& packet) {
    QString path;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path;

    QString errorMessage;
    if (m_fileManager->deleteFile(path, errorMessage)) {
        sendSuccess();
    } else {
        sendError(errorMessage);
    }
}

void Session::handleRename(const Packet& packet) {
    QString oldPath, newPath;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> oldPath >> newPath;

    QString errorMessage;
    if (m_fileManager->rename(oldPath, newPath, errorMessage)) {
        sendSuccess();
    } else {
        sendError(errorMessage);
    }
}

void Session::handleChangeDir(const Packet& packet) {
    QString path;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path;

    const QFileInfo info(m_fileManager->absolutePath(path));
    const bool ok = info.exists() && info.isDir();

    Packet resp(Command::DIR_RESPONSE);
    QDataStream out(&resp.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << ok << path << (ok ? QString() : QStringLiteral("目录不存在"));
    sendPacket(resp);
}

void Session::handleUploadStart(const Packet& packet) {
    QString path;
    qint64 fileSize = 0;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path >> fileSize;

    qint64 resumeOffset = 0;
    const QString token = m_dataChannelServer->registerPendingTransfer(path, true, fileSize, resumeOffset);

    Packet resp(Command::TRANSFER_READY);
    QDataStream out(&resp.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << true << QString() << token << resumeOffset;
    sendPacket(resp);
}

void Session::handleDownloadStart(const Packet& packet) {
    QString path;
    qint64 localExistingSize = 0;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> path >> localExistingSize;

    Packet resp(Command::TRANSFER_READY);
    QDataStream out(&resp.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);

    if (!m_fileManager->exists(path)) {
        out << false << QStringLiteral("文件不存在") << QString() << static_cast<qint64>(0);
        sendPacket(resp);
        return;
    }

    qint64 resumeOffset = 0;
    const QString token = m_dataChannelServer->registerPendingTransfer(path, false, localExistingSize, resumeOffset);
    out << true << QString() << token << resumeOffset;
    sendPacket(resp);
}

void Session::handleTransferCancel(const Packet& packet) {
    QString token;
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);
    in >> token;

    m_dataChannelServer->cancelTransfer(token);
    sendSuccess();
}
