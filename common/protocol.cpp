#include "protocol.h"
#include <QBuffer>

namespace FTP {

QByteArray FileInfo::serialize() const {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << name << size << isDirectory << modifiedTime << permissions;
    return data;
}

FileInfo FileInfo::deserialize(const QByteArray& data) {
    FileInfo info;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_0);
    stream >> info.name >> info.size >> info.isDirectory >> info.modifiedTime >> info.permissions;
    return info;
}

QByteArray Packet::serialize() const {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    
    // 写入魔数和版本
    stream << (quint32)0x46545001;  // "FTP" + version 1
    stream << static_cast<quint8>(command);
    stream << payload;
    
    return data;
}

Packet Packet::deserialize(const QByteArray& data) {
    Packet packet;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_0);
    
    quint32 magic;
    quint8 cmd;
    
    stream >> magic;
    if (magic != 0x46545001) {
        packet.command = Command::ERROR;
        return packet;
    }
    
    stream >> cmd;
    packet.command = static_cast<Command>(cmd);
    stream >> packet.payload;
    
    return packet;
}

QByteArray Packet::createDataPacket(quint64 offset, const QByteArray& data) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << offset << data;
    return payload;
}

bool Packet::parseDataPacket(const QByteArray& payload, quint64& offset, QByteArray& data) {
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    stream >> offset >> data;
    return stream.status() == QDataStream::Ok;
}

void writeFileInfoList(QDataStream& stream, const QList<FileInfo>& list) {
    stream << static_cast<qint32>(list.size());
    for (const FileInfo& info : list) {
        stream << info.name << info.size << info.isDirectory << info.modifiedTime << info.permissions;
    }
}

QList<FileInfo> readFileInfoList(QDataStream& stream) {
    qint32 count = 0;
    stream >> count;
    QList<FileInfo> list;
    list.reserve(qMax(count, 0));
    for (qint32 i = 0; i < count; ++i) {
        FileInfo info;
        stream >> info.name >> info.size >> info.isDirectory >> info.modifiedTime >> info.permissions;
        list.append(info);
    }
    return list;
}

void writeFramedPacket(QIODevice* device, const Packet& packet) {
    const QByteArray body = packet.serialize();
    QByteArray frame;
    QDataStream out(&frame, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << static_cast<quint32>(body.size());
    frame.append(body);
    device->write(frame);
}

void PacketFramer::feed(const QByteArray& data) {
    m_buffer.append(data);
}

bool PacketFramer::hasPacket() const {
    if (m_buffer.size() < static_cast<int>(sizeof(quint32))) {
        return false;
    }
    QDataStream in(m_buffer);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 len = 0;
    in >> len;
    return static_cast<quint64>(m_buffer.size()) >= sizeof(quint32) + static_cast<quint64>(len);
}

Packet PacketFramer::takePacket() {
    QDataStream in(m_buffer);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 len = 0;
    in >> len;
    const QByteArray body = m_buffer.mid(sizeof(quint32), len);
    m_buffer.remove(0, sizeof(quint32) + len);
    return Packet::deserialize(body);
}

} // namespace FTP
