#include "udp_socket.h"

#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace core {

namespace {

constexpr native_socket_t kInvalidSocket =
#ifdef _WIN32
    INVALID_SOCKET;
#else
    -1;
#endif

// 和 socket.cpp 里的同名函数是同一个幂等一次性初始化(函数内 static 局部变量,
// C++11 保证线程安全),两边各自持有一份是有意的——core/platform/ 下每个文件
// 保持自包含,不为了共享几行代码去新增一个内部头文件。
void ensurePlatformInit() {
    static SocketPlatformInit initGuard;
    (void)initGuard;
}

} // namespace

UdpSocket::UdpSocket() : m_fd(kInvalidSocket) {
    ensurePlatformInit();
    m_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

UdpSocket::UdpSocket(native_socket_t fd) : m_fd(fd) {
    ensurePlatformInit();
}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : m_fd(other.m_fd) {
    other.m_fd = kInvalidSocket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_fd = other.m_fd;
        other.m_fd = kInvalidSocket;
    }
    return *this;
}

bool UdpSocket::isValid() const {
    return m_fd != kInvalidSocket;
}

void UdpSocket::close() {
    if (isValid()) {
#ifdef _WIN32
        ::closesocket(m_fd);
#else
        ::close(m_fd);
#endif
        m_fd = kInvalidSocket;
    }
}

bool UdpSocket::bind(uint16_t port) {
    if (!isValid()) return false;

    const int reuse = 1;
    ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    return ::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::setBroadcast(bool enable) {
    if (!isValid()) return false;
    const int value = enable ? 1 : 0;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

bool UdpSocket::sendTo(const std::string& host, uint16_t port, const void* data, size_t len) {
    if (!isValid()) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return false;

#ifdef _WIN32
    const int n = ::sendto(m_fd, static_cast<const char*>(data), static_cast<int>(len), 0,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(len);
#else
    for (;;) {
        const long n = ::sendto(m_fd, data, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        return n == static_cast<long>(len);
    }
#endif
}

long UdpSocket::recvFrom(void* buf, size_t len, std::string& outFromAddress, uint16_t& outFromPort, int timeoutMs) {
    if (!isValid()) return -1;

    if (timeoutMs >= 0) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(m_fd, &set);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int rc = ::select(static_cast<int>(m_fd) + 1, &set, nullptr, nullptr, &tv);
        if (rc == 0) return -2; // 超时
        if (rc < 0) return -1;
    }

    sockaddr_in fromAddr{};
    socklen_t fromLen = sizeof(fromAddr);

#ifdef _WIN32
    const int n = ::recvfrom(m_fd, static_cast<char*>(buf), static_cast<int>(len), 0,
                              reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
    if (n == SOCKET_ERROR) return -1;
#else
    long n = 0;
    for (;;) {
        n = ::recvfrom(m_fd, buf, len, 0, reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    if (n < 0) return -1;
#endif

    char ipBuf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &fromAddr.sin_addr, ipBuf, sizeof(ipBuf)) != nullptr) {
        outFromAddress = ipBuf;
    } else {
        outFromAddress.clear();
    }
    outFromPort = ntohs(fromAddr.sin_port);
    return n;
}

} // namespace core
