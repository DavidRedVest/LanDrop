#include "ftp_server.h"

#include "ftp_session.h"

#include <algorithm>
#include <exception>

namespace core {

namespace {
constexpr int kAcceptPollTimeoutMs = 500; // 让 acceptLoop 定期醒来检查 m_running
} // namespace

FtpServer::FtpServer() = default;

FtpServer::~FtpServer() {
    stop();
}

bool FtpServer::start(uint16_t port) {
    if (m_running.load()) return false;
    if (!m_listenSocket.bindAndListen(port)) return false;
    m_running = true;
    m_acceptThread = std::thread(&FtpServer::acceptLoop, this);
    return true;
}

void FtpServer::stop() {
    if (!m_running.exchange(false)) return;
    m_listenSocket.close(); // 尽快唤醒可能卡在 accept() 里的 accept 线程
    if (m_acceptThread.joinable()) m_acceptThread.join();

    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    m_sessions.clear(); // 每个 FtpSession 的析构会 requestStop() 并 join 自己的线程
}

int FtpServer::activeSessionCount() const {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    return static_cast<int>(
        std::count_if(m_sessions.begin(), m_sessions.end(), [](const std::unique_ptr<FtpSession>& s) {
            return !s->isFinished();
        }));
}

void FtpServer::acceptLoop() {
    while (m_running.load()) {
        try {
            Socket client;
            if (!m_listenSocket.accept(client, kAcceptPollTimeoutMs)) continue; // 超时或停止,重新检查 m_running

            SessionCallbacks callbacks{m_authenticator,    m_rootPathResolver,   m_onLog,
                                       m_onTransferStarted, m_onTransferProgress, m_onTransferCompleted};
            auto session = std::unique_ptr<FtpSession>(new FtpSession(std::move(client), m_rootPath, callbacks));
            session->start();

            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
                                             [](const std::unique_ptr<FtpSession>& s) { return s->isFinished(); }),
                              m_sessions.end());
            m_sessions.push_back(std::move(session));
        } catch (const std::exception&) {
            // 同 FtpSession::run() 的道理:这是 accept 线程的顶层循环,任何未捕获
            // 异常(比如极端资源不足时 new/std::thread 构造抛出)逃出去都会
            // std::terminate() 整个进程——吞掉,跳过这一次 accept,继续服务其它
            // 客户端,而不是让一次偶发的资源问题拖垮整个服务端。
        } catch (...) {
        }
    }
}

} // namespace core
