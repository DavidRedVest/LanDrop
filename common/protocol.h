#pragma once

#include <QByteArray>
#include <QString>
#include <QDataStream>
#include <QIODevice>
#include <QDateTime>
#include <QList>

namespace FTP {

// 协议命令枚举
//
// 每个命令的 payload 都是一个 QDataStream(版本 Qt_6_0)写出的字段序列,
// 字段顺序如下(未注明则表示该命令没有 payload):
//
//   LOGIN            username(QString), password(QString)
//   LOGIN_RESP       success(bool), message(QString)
//   LOGOUT           (无 payload)
//   LIST_FILES       path(QString)                                    — 相对根目录的路径
//   FILE_LIST        success(bool), message(QString), path(QString), 文件列表(见 writeFileInfoList)
//   UPLOAD_START     path(QString), fileSize(qint64)                  — 控制通道,客户端发起上传,
//                    fileSize 是本地源文件总大小
//   DOWNLOAD_START   path(QString), localExistingSize(qint64)         — 控制通道,客户端发起下载,
//                    localExistingSize 是客户端本地已有的字节数(未续传则为 0)
//   TRANSFER_READY   success(bool), message(QString), token(QString), resumeOffset(qint64)
//                    — 控制通道,服务端对 UPLOAD_START/DOWNLOAD_START 的响应;
//                    上传时 resumeOffset 是服务端已有的字节数(客户端应跳过这么多字节);
//                    下载时 resumeOffset 是服务端校验/钳制后实际会开始发送的偏移
//   DATA_CHANNEL_AUTH  token(QString)                                 — 数据通道上,客户端用来认领一次传输
//   TRANSFER_DATA    见 Packet::createDataPacket/parseDataPacket (offset + 数据块),数据通道上重复发送
//   TRANSFER_ACK     bytesAcked(quint64)                              — 预留,phase 1 未使用(依赖 TCP 自身的流控)
//   TRANSFER_DONE    sha256(QByteArray, 32 字节)                       — 数据通道,发送端发完最后一块后附带整文件哈希
//   TRANSFER_ERROR   message(QString)
//   TRANSFER_CANCEL  token(QString)                                   — 控制通道,取消一个进行中的传输
//   MKDIR / RMDIR / DELETE_FILE   path(QString)
//   RENAME           oldPath(QString), newPath(QString)
//   CHANGE_DIR       path(QString)                                    — 仅用于校验路径是否存在,服务端不保存会话级 cwd
//   CURRENT_DIR      (无 payload,phase 1 未使用:客户端自行维护当前远程路径)
//   DIR_RESPONSE     success(bool), canonicalPath(QString), message(QString)
//   ERROR            message(QString)
//   SUCCESS          message(QString,可为空)
//   PING / PONG      (无 payload)
enum class Command : quint8 {
    // 连接控制
    LOGIN           = 0x01,     // 登录请求
    LOGIN_RESP      = 0x02,     // 登录响应
    LOGOUT          = 0x03,     // 登出

    // 文件操作
    LIST_FILES      = 0x10,     // 获取文件列表
    FILE_LIST       = 0x11,     // 文件列表响应

    // 传输控制
    UPLOAD_START    = 0x20,     // 开始上传
    DOWNLOAD_START  = 0x21,     // 开始下载
    TRANSFER_READY  = 0x22,     // 传输准备就绪
    TRANSFER_DATA   = 0x23,     // 传输数据块
    TRANSFER_ACK    = 0x24,     // 数据块确认
    TRANSFER_DONE   = 0x25,     // 传输完成
    TRANSFER_ERROR  = 0x26,     // 传输错误
    TRANSFER_CANCEL = 0x27,     // 取消传输
    DATA_CHANNEL_AUTH = 0x28,   // 数据通道:用一次性 token 认领传输

    // 文件管理
    MKDIR           = 0x30,     // 创建目录
    RMDIR           = 0x31,     // 删除目录
    DELETE_FILE     = 0x32,     // 删除文件
    RENAME          = 0x33,     // 重命名

    // 路径操作
    CHANGE_DIR      = 0x40,     // 切换目录
    CURRENT_DIR     = 0x41,     // 获取当前目录
    DIR_RESPONSE    = 0x42,     // 目录响应

    // 状态
    ERROR           = 0xF0,     // 通用错误
    SUCCESS         = 0xF1,     // 通用成功
    PING            = 0xFE,     // 心跳
    PONG            = 0xFF      // 心跳响应
};

// 文件信息结构
struct FileInfo {
    QString name;
    qint64 size;
    bool isDirectory;
    QDateTime modifiedTime;
    QString permissions;

    FileInfo() : size(0), isDirectory(false) {}

    QByteArray serialize() const;
    static FileInfo deserialize(const QByteArray& data);
};

// 把一组 FileInfo 写入/读出一个已经打开的 QDataStream(用于拼装 FILE_LIST payload)
void writeFileInfoList(QDataStream& stream, const QList<FileInfo>& list);
QList<FileInfo> readFileInfoList(QDataStream& stream);

// 传输信息结构
struct TransferInfo {
    QString sourcePath;
    QString targetPath;
    qint64 fileSize;
    qint64 bytesTransferred;
    qint64 bytesPerSecond;
    int progressPercent;
    bool isUpload;
    bool isActive;
    QString status;
    QString errorMessage;

    TransferInfo()
        : fileSize(0)
        , bytesTransferred(0)
        , bytesPerSecond(0)
        , progressPercent(0)
        , isUpload(false)
        , isActive(false)
    {}
};

// 协议数据包
struct Packet {
    Command command;
    QByteArray payload;

    Packet(Command cmd = Command::SUCCESS) : command(cmd) {}

    QByteArray serialize() const;
    static Packet deserialize(const QByteArray& data);
    static QByteArray createDataPacket(quint64 offset, const QByteArray& data);
    static bool parseDataPacket(const QByteArray& payload, quint64& offset, QByteArray& data);
};

// 把一个 Packet 写到 QIODevice(如 QTcpSocket)上,前面加一个 4 字节长度前缀,
// 这样接收端可以在字节到齐之前先缓冲,而不是直接把流式 socket 交给 QDataStream。
void writeFramedPacket(QIODevice* device, const Packet& packet);

// 增量帧解析器:喂入 socket 读到的原始字节,readyReadPacket() 表示 buffer
// 里已经攒够一个完整帧,可以调用 takePacket() 取出(会从 buffer 中移除这部分数据)。
class PacketFramer {
public:
    void feed(const QByteArray& data);
    bool hasPacket() const;
    Packet takePacket();

private:
    QByteArray m_buffer;
};

// 常量定义
constexpr int DEFAULT_PORT = 2121;
constexpr int DATA_PORT = 2122;
constexpr int MAX_PACKET_SIZE = 524288;
constexpr int BLOCK_SIZE = 262144;
constexpr int TRANSFER_TIMEOUT_MS = 30000;

} // namespace FTP
