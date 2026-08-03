#pragma once

// 跨平台原生 UDP socket 封装,和同目录 socket.h 的 TCP Socket 是同一种设计风格
// (RAII、不可拷贝可移动、不内置应用层策略),只是换成 UDP 语义:没有连接/accept,
// 是 sendTo/recvFrom;局域网自动发现(core/discovery/)专用,禁止出现任何 Qt 类型。

#include "socket.h" // 复用 native_socket_t 类型别名和 SocketPlatformInit

#include <cstddef>
#include <cstdint>
#include <string>

namespace core {

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    // 绑定到 INADDR_ANY:port。接收方(DiscoveryListener)必须先 bind 才能收到广播;
    // 只发送的一方(DiscoveryBeacon)可以跳过 bind,系统会在第一次 sendTo 时隐式
    // 分配一个临时源端口。
    bool bind(uint16_t port);

    // 允许发送广播包(设置 SO_BROADCAST)——不设置的话,sendTo 目标是广播地址
    // (如 255.255.255.255)会直接失败,这是 BSD/Winsock 的共同行为,不是 bug。
    bool setBroadcast(bool enable);

    bool sendTo(const std::string& host, uint16_t port, const void* data, size_t len);

    // 接收一个数据报。返回值:>0 实际收到的字节数;0 表示收到一个空数据报(UDP
    // 下合法,不代表连接关闭,和 TCP 的 recvSome 语义不同,调用方不该当错误处理);
    // -1 出错;-2 超时(timeoutMs 到期还没收到)。outFromAddress/outFromPort 是
    // 发送方的源地址,用来在发现列表里给每个设备去重。
    long recvFrom(void* buf, size_t len, std::string& outFromAddress, uint16_t& outFromPort, int timeoutMs);

    void close();
    bool isValid() const;

private:
    explicit UdpSocket(native_socket_t fd);

    native_socket_t m_fd;
};

} // namespace core
