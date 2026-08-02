#pragma once

// 真实 FTP 客户端(RFC 959)协议状态机,构建于 core::Socket / core::Heartbeat 之上。
// 不依赖 Qt——这是核心层,GUI 侧(client/)之后会写一个薄适配层把这里的回调转成
// Qt 信号(见重构计划阶段 D)。

#include "../callbacks.h"
#include "../heartbeat.h"
#include "../platform/socket.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace core {

struct FtpFileEntry {
    std::string name;
    bool isDirectory = false;
    uint64_t size = 0;
};

enum class FtpResult {
    Ok,
    NotConnected,
    ConnectFailed,
    LoginFailed,
    CommandFailed,
    DataChannelFailed,
    IoError,
    Cancelled,
    Timeout,
};

// 全部公开方法都是同步阻塞调用——调用方(未来 Phase D 的 Qt 适配层)负责把它们放到
// 自己的 std::thread 上跑,不要在 UI 线程直接调用。这与现有 GUI 代码里
// ClientTransferWorker 跑在独立 QThread 上是同一条边界,只是这里换成了标准
// std::thread,不依赖 QThread 或 Qt 事件循环。
//
// 心跳:内部持有一条独立线程(core::Heartbeat),但只在"控制通道确实空闲"时才发送
// NOOP——RFC 959 的控制连接是严格请求/响应配对的协议,数据传输期间控制通道的
// "命令槽"被 RETR/STOR 占着(在等最终的 226 响应),这时候插入 NOOP 会和真正的
// 响应交错、读出乱码,因此心跳在 beginTransfer()/endTransfer() 之间自动暂停,只
// 覆盖"已登录、正在浏览或空闲"的窗口——这正是长时间大文件传输期间,数据通道繁忙
// 但控制通道本身闲置、容易被 NAT/防火墙判定超时挂断的场景之外的部分。见 .cpp 里
// sendHeartbeatNoop() 的注释。
class FtpClient {
public:
    FtpClient();
    ~FtpClient();

    FtpClient(const FtpClient&) = delete;
    FtpClient& operator=(const FtpClient&) = delete;

    void setProgressCallback(ProgressCallback cb) { m_onProgress = std::move(cb); }
    void setStateCallback(StateCallback cb) { m_onState = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { m_onError = std::move(cb); }
    void setLogCallback(LogCallback cb) { m_onLog = std::move(cb); }

    FtpResult connect(const std::string& host, uint16_t port, int timeoutMs = 5000);
    FtpResult login(const std::string& username, const std::string& password);
    // 尽力发送 QUIT 并关闭连接;从任何状态调用都安全(包括从未连接成功时)。
    void disconnect();
    bool isConnected() const { return m_control.isValid(); }

    FtpResult pwd(std::string& outPath);
    FtpResult cwd(const std::string& path);
    FtpResult mkdir(const std::string& path);
    FtpResult rmdir(const std::string& path);
    FtpResult removeFile(const std::string& path);
    FtpResult rename(const std::string& fromPath, const std::string& toPath);
    // 优先用 MLSD(通过 FEAT 探测服务端是否支持),否则退回解析 LIST 的类 Unix
    // ls -l 文本格式(没有严格标准,尽力而为)。
    FtpResult list(const std::string& path, std::vector<FtpFileEntry>& outEntries);

    // resumeOffset > 0 时用 REST 续传。分块读写(见 .cpp 里 kChunkSize),数据通道
    // 用 select()(Socket::waitReadable/waitWritable)等待,绝不整文件读入内存,
    // 每块之间检查取消标志。
    FtpResult downloadFile(const std::string& remotePath, const std::string& localPath, uint64_t resumeOffset = 0);
    FtpResult uploadFile(const std::string& localPath, const std::string& remotePath, uint64_t resumeOffset = 0);

    // 请求取消当前正在进行的 downloadFile/uploadFile;由传输循环轮询
    // std::atomic<bool> 标志位实现,线程安全,可以从任意线程调用。
    void cancel() { m_cancelRequested = true; }

    // 纯函数解析工具,不依赖任何实例状态——公开成 static 是为了能在没有真实 FTP
    // 服务器的情况下被独立单元测试覆盖(见 core/tests/ftp_client_parse_test.cpp)。
    static bool parsePasvReply(const std::string& message, std::string& outIp, uint16_t& outPort);
    static void parseListLine(const std::string& line, bool mlsd, std::vector<FtpFileEntry>& outEntries);

private:
    bool readLine(std::string& outLine, int timeoutMs);
    bool readReply(int& outCode, std::string& outMessage, int timeoutMs = 15000);
    // 发送一行命令(自动加 \r\n)并读取回复,整个过程持有 m_controlMutex,防止和
    // 心跳线程的 NOOP 交错写坏控制通道。
    bool command(const std::string& line, int& outCode, std::string& outMessage, int timeoutMs = 15000);

    void detectMlsdSupport();
    bool enterPassiveMode(std::string& outIp, uint16_t& outPort);

    void startHeartbeat();
    bool sendHeartbeatNoop();
    void beginTransfer();
    void endTransfer();

    void setState(TransferState state);
    void reportError(const std::string& message);
    void log(const std::string& message);

    Socket m_control;
    std::string m_recvBuffer;
    std::mutex m_controlMutex;

    std::unique_ptr<Heartbeat> m_heartbeat;
    std::atomic<bool> m_transferActive{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_loggedIn{false};

    bool m_featChecked = false;
    bool m_mlsdSupported = false;

    ProgressCallback m_onProgress;
    StateCallback m_onState;
    ErrorCallback m_onError;
    LogCallback m_onLog;
};

} // namespace core
