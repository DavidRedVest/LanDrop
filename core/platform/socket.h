#pragma once

// 跨平台原生 socket 封装。禁止在这一层出现任何 Qt 类型——这是"核心"的最底层,
// 只依赖标准 C++ 和操作系统 socket API。Windows 用 winsock2,其余平台用 BSD socket。

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
// windows.h(被 winsock2.h 间接拉进来)默认会定义 min/max 宏,和 <algorithm> 里的
// std::min/std::max 撞名——任何 core/ 下的代码只要写 std::min(...)/std::max(...),
// 在 Windows 上不定义 NOMINMAX 就会被预处理器错误展开,产生 "illegal token on
// right side of '::'" 这种看起来毫不相关的编译错误(真实在 Windows CI 上炸过一次:
// core/ftp/ftp_transfer_manager.cpp 里 std::min(retryCount, kMaxRetryBackoffSteps)
// 编译失败,macOS/Linux 上完全不会复现,因为它们不会拉进 windows.h)。这里是整个
// core/ 唯一会 #include <winsock2.h> 的地方(其余文件都通过包含这个头间接引入),
// 在这里统一定义一次,不需要每个用到 std::min/max 的文件都各自小心。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
    // 注意:非阻塞模式下写缓冲区满时,这个函数内部会以 timeoutMs=-1(无限等待)
    // 调用 waitWritable() ——也就是说它本身不可被外部取消标志中断。需要在长时间
    // 传输中支持"取消"/超时检测的调用方(比如 FtpClient 的分块上传循环),应该改用
    // 下面的 sendSome() 自己实现"发送一块、检查取消标志、必要时带超时地等可写"的循环,
    // 而不是直接指望 sendAll 在这种场景下能被打断。
    bool sendAll(const void* data, size_t len);

    // 读取最多 len 字节。返回值:>0 实际读到的字节数;0 表示对端正常关闭连接;
    // -1 表示出错;-2 表示当前是非阻塞模式且暂时没有数据(调用方应该
    // waitReadable() 之后重试)。
    long recvSome(void* buf, size_t len);

    // 发送最多 len 字节,可能只发出一部分(比如写缓冲区满)。与 recvSome 对称的
    // 语义:>=0 实际发送的字节数;-1 出错;-2 非阻塞模式下暂时不可写(调用方应该
    // waitWritable() 之后重试,期间可以检查取消标志/做超时判断)。
    long sendSome(const void* data, size_t len);

    void setNonBlocking(bool enable);
    void close();
    bool isValid() const;
    native_socket_t nativeHandle() const { return m_fd; }

    // 基于 select() 的可读/可写等待,timeoutMs < 0 表示无限等待。
    WaitResult waitReadable(int timeoutMs) const;
    WaitResult waitWritable(int timeoutMs) const;

    std::string peerAddress() const;
    // 本端(getsockname)的 IP——PASV 用它拼被动模式回复里的地址,即"客户端连过来
    // 的那个接口地址",而不是随便取一个本机 IP(多网卡时可能猜错)。
    std::string localAddress() const;
    uint16_t localPort() const;

private:
    explicit Socket(native_socket_t fd);
    void applyPlatformSocketOptions();

    native_socket_t m_fd;
};

} // namespace core
