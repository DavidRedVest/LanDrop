#pragma once

// 跨平台原生 socket 封装。禁止在这一层出现任何 Qt 类型——这是"核心"的最底层,
// 只依赖标准 C++ 和操作系统 socket API。Windows 用 winsock2,其余平台用 BSD socket。

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace core {

#ifdef _WIN32
using native_socket_t = SOCKET;
#else
using native_socket_t = int;
#endif

// 进程级 Winsock 初始化(RAII)。非 Windows 平台上构造/析构都是空操作。
// 不需要手动使用——Socket 的每个会创建新 fd 的操作都会先确保这个初始化完成过
// (用函数内 static 局部变量实现 C++11 保证的线程安全一次性初始化)。
class SocketPlatformInit {
public:
    SocketPlatformInit();
    ~SocketPlatformInit();
    SocketPlatformInit(const SocketPlatformInit&) = delete;
    SocketPlatformInit& operator=(const SocketPlatformInit&) = delete;
};

enum class WaitResult { Ready, Timeout, Error };

// 跨平台原生 TCP socket 的 RAII 封装:不可拷贝,可移动。
//
// 设计上只提供"正确的原语",不内置任何应用层策略(分块大小、取消检查节奏等
// 留给上层,比如未来的 FtpClient/FtpServer 决定)。
class Socket {
public:
    Socket();
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // 客户端:连接 host:port(host 可以是 IPv4 字面量或主机名),timeoutMs 内完成握手。
    // 内部用非阻塞 connect + waitWritable 实现超时,成功后恢复为阻塞模式。
    bool connectTo(const std::string& host, uint16_t port, int timeoutMs);

    // 服务端:绑定并监听(IPv4 / INADDR_ANY)。
    bool bindAndListen(uint16_t port, int backlog = 16);

    // 服务端:接受一个连接。timeoutMs < 0 表示无限等待;>=0 超时后返回 false。
    bool accept(Socket& outClient, int timeoutMs = -1);

    // 发送 len 字节,内部循环处理部分写入,直到全部发完或出错(返回 false)。
    bool sendAll(const void* data, size_t len);

    // 读取最多 len 字节。返回值:>0 实际读到的字节数;0 表示对端正常关闭连接;
    // -1 表示出错;-2 表示当前是非阻塞模式且暂时没有数据(调用方应该
    // waitReadable() 之后重试)。
    long recvSome(void* buf, size_t len);

    void setNonBlocking(bool enable);
    void close();
    bool isValid() const;
    native_socket_t nativeHandle() const { return m_fd; }

    // 基于 select() 的可读/可写等待,timeoutMs < 0 表示无限等待。
    WaitResult waitReadable(int timeoutMs) const;
    WaitResult waitWritable(int timeoutMs) const;

    std::string peerAddress() const;
    uint16_t localPort() const;

private:
    explicit Socket(native_socket_t fd);
    void applyPlatformSocketOptions();

    native_socket_t m_fd;
};

} // namespace core
