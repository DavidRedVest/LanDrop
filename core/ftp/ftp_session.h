#pragma once

// 一个已 accept 的客户端控制连接的完整会话:登录、PWD/CWD/CDUP/TYPE、MKD/RMD/DELE/
// RNFR+RNTO、PASV+LIST/MLSD、RETR/STOR(+REST 续传)、NOOP、FEAT、QUIT。跑在自己的
// std::thread 上,直到客户端断开或 QUIT。不依赖 Qt。

#include "../platform/socket.h"
#include "ftp_server.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace core {

class FtpSession {
public:
    FtpSession(Socket controlSocket, std::string rootPath, FtpServer::Authenticator authenticator,
               FtpServer::RootPathResolver rootPathResolver, FtpServer::SessionLogCallback onLog);
    ~FtpSession();

    FtpSession(const FtpSession&) = delete;
    FtpSession& operator=(const FtpSession&) = delete;

    void start(); // 启动会话线程
    bool isFinished() const { return m_finished.load(); }
    // 服务端主动关停时调用:强制关闭该会话的 socket 以尽快唤醒可能卡在
    // select()/recv() 里的会话线程。析构函数会自动调用并 join。
    void requestStop();

private:
    void run();
    void log(const std::string& message);

    bool readLine(std::string& outLine, int timeoutMs);
    bool sendReply(int code, const std::string& text);
    bool sendFeat();
    bool acceptDataConnection(Socket& outSocket, int timeoutMs);

    void handleUser(const std::string& arg);
    void handlePass(const std::string& arg);
    void handlePwd();
    void handleCwd(const std::string& arg);
    void handleType(const std::string& arg);
    void handlePasv();
    void handleList(const std::string& arg, bool mlsd);
    void handleRetr(const std::string& arg);
    void handleStor(const std::string& arg);
    void handleRest(const std::string& arg);
    void handleMkd(const std::string& arg);
    void handleRmd(const std::string& arg);
    void handleDele(const std::string& arg);
    void handleRnfr(const std::string& arg);
    void handleRnto(const std::string& arg);

    // 把命令参数(可能相对、可能带前导 "/")结合当前 m_cwd 解析成一个干净的
    // 虚拟绝对路径(没有 "." / ".." 段)。还要再经过 core::normalizeFtpPath 才是
    // 真正落地用的 OS 路径。
    std::string resolveVirtualPath(const std::string& arg) const;
    bool requireLoggedIn();

    Socket m_control;
    Socket m_dataListener; // PASV 打开的一次性监听 socket,每次数据传输后关闭
    std::string m_peerAddress;
    std::string m_rootPath;
    std::string m_cwd = "/";

    FtpServer::Authenticator m_authenticator;
    FtpServer::RootPathResolver m_rootPathResolver;
    FtpServer::SessionLogCallback m_onLog;

    std::thread m_thread;
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_stopRequested{false};

    std::string m_username;
    bool m_loggedIn = false;
    bool m_awaitingPassword = false;
    uint64_t m_restOffset = 0;
    std::string m_renameFrom; // RNFR 记下的虚拟路径,等 RNTO 消费

    std::string m_recvBuffer;
};

} // namespace core
