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

    // 结构化传输事件回调,供 GUI(ServerWindow)展示"哪个会话在传哪个文件、进度
    // 多少"的实时表格,而不用去解析人类可读的日志文本。sessionId 是 FtpSession
    // 内部生成的稳定标识(同一个连接从开始到结束保持不变),不是操作系统句柄。
    // transferred/total 语义和 core::ProgressCallback 一致:STOR(服务端接收上传)
    // 场景下标准 FTP 没有"客户端预先声明文件大小"的机制,total 传 0 表示未知——
    // 这和 FtpClient 自己上传时的约定完全一致,不是新引入的不一致。
    using TransferStartedCallback =
        std::function<void(const std::string& sessionId, const std::string& fileName, bool isUpload)>;
    using TransferProgressCallback =
        std::function<void(const std::string& sessionId, uint64_t transferred, uint64_t total)>;
    using TransferCompletedCallback = std::function<void(const std::string& sessionId, const std::string& fileName)>;

    // 每个会话需要的全部回调打包成一个小结构体,而不是让 FtpSession 的构造函数
    // 一直堆参数——目前已经有 6 个,继续按位置参数加下去容易传错顺序。
    struct SessionCallbacks {
        Authenticator authenticator;
        RootPathResolver rootPathResolver;
        SessionLogCallback onLog;
        TransferStartedCallback onTransferStarted;
        TransferProgressCallback onTransferProgress;
        TransferCompletedCallback onTransferCompleted;
    };

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
    void setTransferStartedCallback(TransferStartedCallback cb) { m_onTransferStarted = std::move(cb); }
    void setTransferProgressCallback(TransferProgressCallback cb) { m_onTransferProgress = std::move(cb); }
    void setTransferCompletedCallback(TransferCompletedCallback cb) { m_onTransferCompleted = std::move(cb); }

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
    TransferStartedCallback m_onTransferStarted;
    TransferProgressCallback m_onTransferProgress;
    TransferCompletedCallback m_onTransferCompleted;

    mutable std::mutex m_sessionsMutex;
    std::vector<std::unique_ptr<FtpSession>> m_sessions;
};

} // namespace core
