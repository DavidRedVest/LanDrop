#include "socket.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
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

void ensurePlatformInit() {
    // C++11 保证函数内 static 局部变量的初始化是线程安全的、只执行一次。
    static SocketPlatformInit initGuard;
    (void)initGuard;
}

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

bool wasInterrupted(int err) {
#ifdef _WIN32
    (void)err;
    return false;
#else
    return err == EINTR;
#endif
}

} // namespace

// ---- SocketPlatformInit ----

SocketPlatformInit::SocketPlatformInit() {
#ifdef _WIN32
    WSADATA wsaData;
    ::WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

SocketPlatformInit::~SocketPlatformInit() {
#ifdef _WIN32
    ::WSACleanup();
#endif
}

// ---- Socket ----

Socket::Socket() : m_fd(kInvalidSocket) {
    ensurePlatformInit();
}

Socket::Socket(native_socket_t fd) : m_fd(fd) {
    ensurePlatformInit();
}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : m_fd(other.m_fd) {
    other.m_fd = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        m_fd = other.m_fd;
        other.m_fd = kInvalidSocket;
    }
    return *this;
}

bool Socket::isValid() const {
    return m_fd != kInvalidSocket;
}

void Socket::close() {
    if (isValid()) {
#ifdef _WIN32
        ::closesocket(m_fd);
#else
        ::close(m_fd);
#endif
        m_fd = kInvalidSocket;
    }
}

void Socket::applyPlatformSocketOptions() {
    if (!isValid()) return;
#if defined(__APPLE__)
    // macOS/BSD 没有 MSG_NOSIGNAL,得靠这个 socket 选项让写入已关闭连接时
    // 返回 EPIPE 而不是杀进程的 SIGPIPE 信号。Linux 用 send() 的 MSG_NOSIGNAL
    // 标志达到同样效果(见 sendAll),Windows 本来就不存在 SIGPIPE 这回事。
    int one = 1;
    ::setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

void Socket::setNonBlocking(bool enable) {
    if (!isValid()) return;
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    ::ioctlsocket(m_fd, FIONBIO, &mode);
#else
    int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags < 0) return;
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ::fcntl(m_fd, F_SETFL, flags);
#endif
}

bool Socket::connectTo(const std::string& host, uint16_t port, int timeoutMs) {
    close();
    ensurePlatformInit();

    m_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!isValid()) return false;
    applyPlatformSocketOptions();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0 || resolved == nullptr) {
            close();
            return false;
        }
        addr = *reinterpret_cast<sockaddr_in*>(resolved->ai_addr);
        addr.sin_port = htons(port);
        ::freeaddrinfo(resolved);
    }

    setNonBlocking(true);
    const int rc = ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        setNonBlocking(false);
        return true;
    }

    const int err = lastSocketError();
    const bool inProgress =
#ifdef _WIN32
        (err == WSAEWOULDBLOCK);
#else
        (err == EINPROGRESS);
#endif
    if (!inProgress) {
        close();
        return false;
    }

    if (waitWritable(timeoutMs) != WaitResult::Ready) {
        close();
        return false;
    }

    int soErr = 0;
    socklen_t soErrLen = sizeof(soErr);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soErrLen) != 0 ||
        soErr != 0) {
        close();
        return false;
    }

    setNonBlocking(false);
    return true;
}

bool Socket::bindAndListen(uint16_t port, int backlog) {
    close();
    ensurePlatformInit();

    m_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!isValid()) return false;
    applyPlatformSocketOptions();

    const int reuse = 1;
    ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close();
        return false;
    }
    if (::listen(m_fd, backlog) != 0) {
        close();
        return false;
    }
    return true;
}

bool Socket::accept(Socket& outClient, int timeoutMs) {
    if (!isValid()) return false;

    if (timeoutMs >= 0) {
        if (waitReadable(timeoutMs) != WaitResult::Ready) return false;
    }

    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
    const native_socket_t clientFd =
        ::accept(m_fd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (clientFd == kInvalidSocket) return false;

    outClient.close();
    outClient.m_fd = clientFd;
    outClient.applyPlatformSocketOptions();
    return true;
}

bool Socket::sendAll(const void* data, size_t len) {
    if (!isValid()) return false;
    const char* p = static_cast<const char*>(data);
    size_t remaining = len;

    while (remaining > 0) {
#if defined(_WIN32)
        const int chunk = remaining > 0x7fffffff ? 0x7fffffff : static_cast<int>(remaining);
        const int n = ::send(m_fd, p, chunk, 0);
        if (n == SOCKET_ERROR) {
            const int err = lastSocketError();
            if (wouldBlock(err)) {
                if (waitWritable(-1) != WaitResult::Ready) return false;
                continue;
            }
            return false;
        }
#elif defined(__APPLE__)
        const long n = ::send(m_fd, p, remaining, 0); // SO_NOSIGPIPE already set on this fd
        if (n < 0) {
            const int err = lastSocketError();
            if (wasInterrupted(err)) continue;
            if (wouldBlock(err)) {
                if (waitWritable(-1) != WaitResult::Ready) return false;
                continue;
            }
            return false;
        }
#else
        const long n = ::send(m_fd, p, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            const int err = lastSocketError();
            if (wasInterrupted(err)) continue;
            if (wouldBlock(err)) {
                if (waitWritable(-1) != WaitResult::Ready) return false;
                continue;
            }
            return false;
        }
#endif
        if (n == 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

long Socket::recvSome(void* buf, size_t len) {
    if (!isValid()) return -1;
#ifdef _WIN32
    const int chunk = len > 0x7fffffff ? 0x7fffffff : static_cast<int>(len);
    const int n = ::recv(m_fd, static_cast<char*>(buf), chunk, 0);
    if (n == SOCKET_ERROR) {
        const int err = lastSocketError();
        if (wouldBlock(err)) return -2;
        return -1;
    }
    return n;
#else
    for (;;) {
        const long n = ::recv(m_fd, buf, len, 0);
        if (n < 0) {
            const int err = lastSocketError();
            if (wasInterrupted(err)) continue;
            if (wouldBlock(err)) return -2;
            return -1;
        }
        return n;
    }
#endif
}

long Socket::sendSome(const void* data, size_t len) {
    if (!isValid()) return -1;
#ifdef _WIN32
    const int chunk = len > 0x7fffffff ? 0x7fffffff : static_cast<int>(len);
    const int n = ::send(m_fd, static_cast<const char*>(data), chunk, 0);
    if (n == SOCKET_ERROR) {
        const int err = lastSocketError();
        if (wouldBlock(err)) return -2;
        return -1;
    }
    return n;
#else
    for (;;) {
        const long n =
#if defined(__APPLE__)
            ::send(m_fd, data, len, 0); // SO_NOSIGPIPE already set on this fd
#else
            ::send(m_fd, data, len, MSG_NOSIGNAL);
#endif
        if (n < 0) {
            const int err = lastSocketError();
            if (wasInterrupted(err)) continue;
            if (wouldBlock(err)) return -2;
            return -1;
        }
        return n;
    }
#endif
}

WaitResult Socket::waitReadable(int timeoutMs) const {
    if (!isValid()) return WaitResult::Error;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(m_fd, &set);

    timeval tv{};
    timeval* tvPtr = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        tvPtr = &tv;
    }

    const int rc = ::select(static_cast<int>(m_fd) + 1, &set, nullptr, nullptr, tvPtr);
    if (rc < 0) return WaitResult::Error;
    if (rc == 0) return WaitResult::Timeout;
    return WaitResult::Ready;
}

WaitResult Socket::waitWritable(int timeoutMs) const {
    if (!isValid()) return WaitResult::Error;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(m_fd, &set);

    timeval tv{};
    timeval* tvPtr = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        tvPtr = &tv;
    }

    const int rc = ::select(static_cast<int>(m_fd) + 1, nullptr, &set, nullptr, tvPtr);
    if (rc < 0) return WaitResult::Error;
    if (rc == 0) return WaitResult::Timeout;
    return WaitResult::Ready;
}

std::string Socket::peerAddress() const {
    if (!isValid()) return {};
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) return {};
    return std::string(buf);
}

uint16_t Socket::localPort() const {
    if (!isValid()) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

} // namespace core
