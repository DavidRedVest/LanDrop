#pragma once

// 真实 FTP 服务端(RFC 959)核心,构建于 core::Socket 之上,不依赖 Qt。
// accept 线程 + 每个客户端一条会话线程(FtpSession)的模型——局域网个人使用规模,
// 简单正确,不需要线程池。

#include "../platform/socket.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

class FtpSession;

class FtpServer {
public:
    using Authenticator = std::function<bool(const std::string& username, const std::string& password)>;
    // 每个已登录会话可选的独立根目录(比如按用户名隔离);返回空字符串表示继续用
    // setRootPath() 设置的全局根目录。不设置这个回调也完全可以。
    using RootPathResolver = std::function<std::string(const std::string& username)>;
    using SessionLogCallback = std::function<void(const std::string& peerAddress, const std::string& message)>;

    FtpServer();
    ~FtpServer();

    FtpServer(const FtpServer&) = delete;
    FtpServer& operator=(const FtpServer&) = delete;

    void setRootPath(std::string path) { m_rootPath = std::move(path); }
    // 未设置 Authenticator 时,任何登录都会被拒绝(安全默认值:宁可拒绝服务,也不能
    // 意外变成一个不需要密码的开放服务器)。
    void setAuthenticator(Authenticator authenticator) { m_authenticator = std::move(authenticator); }
    void setRootPathResolver(RootPathResolver resolver) { m_rootPathResolver = std::move(resolver); }
    void setLogCallback(SessionLogCallback cb) { m_onLog = std::move(cb); }

    // 监听 port(控制通道)。PASV 的数据端口是按需临时分配的(bindAndListen(0) 让
    // 操作系统挑一个空闲端口,通过 getsockname 上报),不是固定的 port+1。
    bool start(uint16_t port);
    void stop();
    bool isRunning() const { return m_running.load(); }
    int activeSessionCount() const;

private:
    void acceptLoop();

    Socket m_listenSocket;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{false};

    std::string m_rootPath;
    Authenticator m_authenticator;
    RootPathResolver m_rootPathResolver;
    SessionLogCallback m_onLog;

    mutable std::mutex m_sessionsMutex;
    std::vector<std::unique_ptr<FtpSession>> m_sessions;
};

} // namespace core
