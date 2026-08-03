#pragma once

// GUI 层(client/、server/)还在用的两个小类型,从旧的 common/protocol.h 里拆出来
// 单独放这——那个文件里的其它内容(Packet/Command/PacketFramer/writeFramedPacket/
// BLOCK_SIZE 等)全部是自定义二进制协议的线格式,只有旧的 common/protocol.*、
// client/connection.*、client/transfer.*、server/session.*、
// server/datachannelserver.*、server/ftpserver.* 还在用,这些文件随着核心迁移到
// 标准 FTP(见 core/ftp/)已经/正在被重写或删除,不应该让新代码继续依赖那个头文件。
//
// FileInfo 和 DEFAULT_PORT 则是纯粹的"GUI 展示用的小数据类型/常量",和具体走
// 哪种线协议无关,继续留着。

#include <QDateTime>
#include <QString>

namespace FTP {

struct FileInfo {
    QString name;
    qint64 size = 0;
    bool isDirectory = false;
    // 新的标准 FTP 核心(core::FtpFileEntry)目前不解析 MLSD 的 Modify 事实或
    // LIST 的时间戳/权限字段(见 core/ftp/ftp_client.cpp 的 parseListLine)——这两个
    // 字段在走新核心时会保持默认(无效/空),文件浏览器对应的两列会显示为空白。
    // 这是标准化后的已知限制,不是这次重写漏掉了什么。
    QDateTime modifiedTime;
    QString permissions;
};

constexpr int DEFAULT_PORT = 2121;

} // namespace FTP
