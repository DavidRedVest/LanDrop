#include "connection.h"

#include <QTcpSocket>
#include <QTimer>

using FTP::Command;
using FTP::Packet;

namespace {
constexpr int kHeartbeatIntervalMs = 15000;
}

Connection::Connection(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &Connection::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &Connection::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &Connection::onSocketError);
    connect(m_socket, &QTcpSocket::readyRead, this, &Connection::onReadyRead);

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &Connection::onHeartbeatTimeout);
}

void Connection::connectToHost(const QString& host, quint16 port) {
    m_host = host;
    m_port = port;
    m_socket->connectToHost(host, port);
}

void Connection::disconnectFromHost() {
    m_heartbeatTimer->stop();
    m_socket->disconnectFromHost();
}

bool Connection::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void Connection::onSocketConnected() {
    m_heartbeatTimer->start(kHeartbeatIntervalMs);
    emit connected();
}

void Connection::onSocketDisconnected() {
    m_heartbeatTimer->stop();
    emit disconnected();
}

void Connection::onSocketError(QAbstractSocket::SocketError) {
    emit connectionError(m_socket->errorString());
}

void Connection::onReadyRead() {
    m_framer.feed(m_socket->readAll());
    while (m_framer.hasPacket()) {
        handlePacket(m_framer.takePacket());
    }
}

void Connection::onHeartbeatTimeout() {
    if (m_pongPending) {
        emit connectionError(QStringLiteral("心跳超时"));
        m_socket->disconnectFromHost();
        return;
    }
    sendPacket(Packet(Command::PING));
    m_pongPending = true;
}

void Connection::sendPacket(const Packet& packet) {
    FTP::writeFramedPacket(m_socket, packet);
}

void Connection::login(const QString& username, const QString& password) {
    Packet p(Command::LOGIN);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << username << password;
    sendPacket(p);
}

void Connection::listDirectory(const QString& path) {
    Packet p(Command::LIST_FILES);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path;
    sendPacket(p);
}

void Connection::mkdir(const QString& path) {
    Packet p(Command::MKDIR);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path;
    sendPacket(p);
}

void Connection::rmdir(const QString& path) {
    Packet p(Command::RMDIR);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path;
    sendPacket(p);
}

void Connection::deleteFile(const QString& path) {
    Packet p(Command::DELETE_FILE);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path;
    sendPacket(p);
}

void Connection::rename(const QString& oldPath, const QString& newPath) {
    Packet p(Command::RENAME);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << oldPath << newPath;
    sendPacket(p);
}

void Connection::changeDir(const QString& path) {
    Packet p(Command::CHANGE_DIR);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path;
    sendPacket(p);
}

void Connection::requestUpload(const QString& path, qint64 fileSize) {
    Packet p(Command::UPLOAD_START);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path << fileSize;
    sendPacket(p);
}

void Connection::requestDownload(const QString& path, qint64 localExistingSize) {
    Packet p(Command::DOWNLOAD_START);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << path << localExistingSize;
    sendPacket(p);
}

void Connection::cancelTransfer(const QString& token) {
    Packet p(Command::TRANSFER_CANCEL);
    QDataStream out(&p.payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << token;
    sendPacket(p);
}

void Connection::handlePacket(const Packet& packet) {
    QDataStream in(packet.payload);
    in.setVersion(QDataStream::Qt_6_0);

    switch (packet.command) {
    case Command::LOGIN_RESP: {
        bool success = false;
        QString message;
        in >> success >> message;
        emit loginResult(success, message);
        break;
    }
    case Command::FILE_LIST: {
        bool success = false;
        QString message, path;
        in >> success >> message >> path;
        QList<FTP::FileInfo> list;
        if (success) {
            list = FTP::readFileInfoList(in);
        }
        emit directoryListed(success, path, message, list);
        break;
    }
    case Command::DIR_RESPONSE: {
        bool success = false;
        QString path, message;
        in >> success >> path >> message;
        emit directoryChanged(success, path, message);
        break;
    }
    case Command::TRANSFER_READY: {
        bool success = false;
        QString message, token;
        qint64 resumeOffset = 0;
        in >> success >> message >> token >> resumeOffset;
        emit transferReady(success, message, token, resumeOffset);
        break;
    }
    case Command::SUCCESS: {
        QString message;
        in >> message;
        emit operationResult(true, message);
        break;
    }
    case Command::ERROR: {
        QString message;
        in >> message;
        emit operationResult(false, message);
        break;
    }
    case Command::PING:
        sendPacket(Packet(Command::PONG));
        break;
    case Command::PONG:
        m_pongPending = false;
        break;
    default:
        break;
    }
}
