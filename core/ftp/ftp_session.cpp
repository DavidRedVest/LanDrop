#include "ftp_session.h"

#include "ftp_paths.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace core {

namespace {
namespace fs = std::filesystem;
constexpr size_t kChunkSize = 262144; // 256KB,和 FtpClient 保持一致
constexpr int kControlIdleTimeoutMs = 300000; // 5 分钟没收到任何命令就断开
constexpr int kDataAcceptTimeoutMs = 15000;
constexpr int kDataStallTimeoutMs = 20000;
constexpr int kProgressThrottleMs = 200; // 和 client/transfer.cpp 的节流间隔保持一致

char toUpperChar(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

std::atomic<uint64_t> g_sessionCounter{0};
} // namespace

FtpSession::FtpSession(Socket controlSocket, std::string rootPath, FtpServer::SessionCallbacks callbacks)
    : m_control(std::move(controlSocket)), m_sessionId("session-" + std::to_string(g_sessionCounter.fetch_add(1) + 1)),
      m_rootPath(std::move(rootPath)), m_authenticator(std::move(callbacks.authenticator)),
      m_rootPathResolver(std::move(callbacks.rootPathResolver)), m_onLog(std::move(callbacks.onLog)),
      m_onTransferStarted(std::move(callbacks.onTransferStarted)),
      m_onTransferProgress(std::move(callbacks.onTransferProgress)),
      m_onTransferCompleted(std::move(callbacks.onTransferCompleted)) {}

FtpSession::~FtpSession() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

void FtpSession::start() {
    m_thread = std::thread(&FtpSession::run, this);
}

void FtpSession::requestStop() {
    m_stopRequested = true;
    // 强制关闭:让可能卡在 select()/recv() 里的会话线程尽快返回。这里存在一个
    // 理论上的 POSIX 竞态(另一个线程正在用这个 fd 做 select 时从别处 close 它,
    // 严格来说是未定义行为的边界情况),但在我们实际支持的平台上都能正常工作;
    // 更稳妥的做法是自管道/eventfd 唤醒 select,这里为了保持简单先不做,
    // kControlIdleTimeoutMs 兜底确保即使这次 close 没生效,线程最终也会退出。
    m_control.close();
    m_dataListener.close();
}

void FtpSession::log(const std::string& message) {
    if (m_onLog) m_onLog(m_peerAddress, message);
}

void FtpSession::reportTransferProgress(uint64_t transferred, uint64_t total,
                                         std::chrono::steady_clock::time_point& lastReportTime) {
    if (!m_onTransferProgress) return;
    const auto now = std::chrono::steady_clock::now();
    // lastReportTime 的默认构造值(epoch)当"还从没报过一次"的哨兵,保证第一块
    // 数据必定被立刻报出去,不用等满 200ms。
    const bool isFirstReport = lastReportTime.time_since_epoch().count() == 0;
    if (!isFirstReport &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReportTime).count() < kProgressThrottleMs) {
        return;
    }
    lastReportTime = now;
    m_onTransferProgress(m_sessionId, transferred, total);
}

// ---- 控制通道读写 ----

bool FtpSession::readLine(std::string& outLine, int timeoutMs) {
    for (;;) {
        const auto pos = m_recvBuffer.find("\r\n");
        if (pos != std::string::npos) {
            outLine = m_recvBuffer.substr(0, pos);
            m_recvBuffer.erase(0, pos + 2);
            return true;
        }
        if (m_stopRequested.load()) return false;
        if (m_control.waitReadable(timeoutMs) != WaitResult::Ready) return false;
        char buf[4096];
        const long n = m_control.recvSome(buf, sizeof(buf));
        if (n <= 0) return false;
        m_recvBuffer.append(buf, static_cast<size_t>(n));
    }
}

bool FtpSession::sendReply(int code, const std::string& text) {
    const std::string wire = std::to_string(code) + " " + text + "\r\n";
    return m_control.sendAll(wire.data(), wire.size());
}

bool FtpSession::sendFeat() {
    const std::string wire = "211-Features:\r\n MLSD\r\n211 End\r\n";
    return m_control.sendAll(wire.data(), wire.size());
}

bool FtpSession::acceptDataConnection(Socket& outSocket, int timeoutMs) {
    if (!m_dataListener.isValid()) return false;
    const bool ok = m_dataListener.accept(outSocket, timeoutMs);
    m_dataListener.close(); // 一次性:每次数据传输前客户端都要重新 PASV
    return ok;
}

// ---- 路径解析 ----

std::string FtpSession::resolveVirtualPath(const std::string& arg) const {
    std::string combined;
    if (!arg.empty() && arg.front() == '/') {
        combined = arg;
    } else {
        combined = m_cwd;
        if (combined.empty() || combined.back() != '/') combined += '/';
        combined += arg;
    }

    std::vector<std::string> parts;
    std::stringstream ss(combined);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(seg);
    }

    std::string result = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        result += parts[i];
        if (i + 1 < parts.size()) result += '/';
    }
    return result;
}

bool FtpSession::requireLoggedIn() {
    if (!m_loggedIn) {
        sendReply(530, "Not logged in");
        return false;
    }
    return true;
}

// ---- 登录 ----

void FtpSession::handleUser(const std::string& arg) {
    m_username = arg;
    m_awaitingPassword = true;
    m_loggedIn = false;
    sendReply(331, "User name okay, need password");
}

void FtpSession::handlePass(const std::string& arg) {
    if (!m_awaitingPassword) {
        sendReply(503, "Login with USER first");
        return;
    }
    m_awaitingPassword = false;

    // 没设置 Authenticator 时一律拒绝——安全默认值,宁可拒绝服务也不能意外变成
    // 不需要密码的开放服务器。
    const bool ok = m_authenticator && m_authenticator(m_username, arg);
    if (!ok) {
        sendReply(530, "Login incorrect");
        log("login failed for user '" + m_username + "'");
        m_username.clear();
        return;
    }

    m_loggedIn = true;
    if (m_rootPathResolver) {
        const std::string resolved = m_rootPathResolver(m_username);
        if (!resolved.empty()) m_rootPath = resolved;
    }
    m_cwd = "/";
    sendReply(230, "Login successful");
    log("login as '" + m_username + "'");
}

// ---- 简单命令 ----

void FtpSession::handlePwd() {
    if (!requireLoggedIn()) return;
    sendReply(257, "\"" + m_cwd + "\" is the current directory");
}

void FtpSession::handleCwd(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string newVirtual = resolveVirtualPath(arg);
    const std::string osPath = normalizeFtpPath(m_rootPath, newVirtual);
    std::error_code ec;
    if (!fs::is_directory(osPath, ec) || ec) {
        sendReply(550, "Failed to change directory: not a directory");
        return;
    }
    m_cwd = newVirtual;
    sendReply(250, "Directory successfully changed");
}

void FtpSession::handleType(const std::string& arg) {
    if (!requireLoggedIn()) return;
    // 只按二进制方式实际处理(不做 ASCII 换行转换,避免损坏非文本文件),但对
    // TYPE A 请求本身仍然回复成功,以兼容那些无条件先发一次 TYPE A 再工作的客户端。
    std::string t = arg;
    std::transform(t.begin(), t.end(), t.begin(), toUpperChar);
    if (t == "I" || t == "A") {
        sendReply(200, "Type set to " + t);
    } else {
        sendReply(504, "Type not supported");
    }
}

void FtpSession::handleMkd(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string newVirtual = resolveVirtualPath(arg);
    const std::string osPath = normalizeFtpPath(m_rootPath, newVirtual);
    const fs::path p(osPath);
    if (!isValidFtpName(p.filename().string())) {
        sendReply(553, "Invalid directory name");
        return;
    }
    std::error_code ec;
    fs::create_directories(osPath, ec);
    if (ec) {
        sendReply(550, "Failed to create directory");
        return;
    }
    sendReply(257, "\"" + newVirtual + "\" directory created");
}

void FtpSession::handleRmd(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string osPath = normalizeFtpPath(m_rootPath, resolveVirtualPath(arg));
    std::error_code ec;
    if (!fs::is_directory(osPath, ec)) {
        sendReply(550, "Directory not found");
        return;
    }
    fs::remove_all(osPath, ec);
    if (ec) {
        sendReply(550, "Failed to remove directory");
        return;
    }
    sendReply(250, "Directory removed");
}

void FtpSession::handleDele(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string osPath = normalizeFtpPath(m_rootPath, resolveVirtualPath(arg));
    std::error_code ec;
    if (!fs::is_regular_file(osPath, ec)) {
        sendReply(550, "File not found");
        return;
    }
    fs::remove(osPath, ec);
    if (ec) {
        sendReply(550, "Failed to delete file");
        return;
    }
    sendReply(250, "File deleted");
}

void FtpSession::handleRnfr(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string virtualPath = resolveVirtualPath(arg);
    const std::string osPath = normalizeFtpPath(m_rootPath, virtualPath);
    std::error_code ec;
    if (!fs::exists(osPath, ec)) {
        sendReply(550, "File/directory not found");
        return;
    }
    m_renameFrom = virtualPath;
    sendReply(350, "Ready for RNTO");
}

void FtpSession::handleRnto(const std::string& arg) {
    if (!requireLoggedIn()) return;
    if (m_renameFrom.empty()) {
        sendReply(503, "RNFR required first");
        return;
    }
    const std::string fromOs = normalizeFtpPath(m_rootPath, m_renameFrom);
    const std::string toVirtual = resolveVirtualPath(arg);
    const std::string toOs = normalizeFtpPath(m_rootPath, toVirtual);
    m_renameFrom.clear();

    const fs::path toP(toOs);
    if (!isValidFtpName(toP.filename().string())) {
        sendReply(553, "Invalid name");
        return;
    }
    std::error_code ec;
    fs::rename(fromOs, toOs, ec);
    if (ec) {
        sendReply(550, "Rename failed");
        return;
    }
    sendReply(250, "Rename successful");
}

void FtpSession::handleRest(const std::string& arg) {
    if (!requireLoggedIn()) return;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(arg.c_str(), &end, 10);
    if (end == arg.c_str()) {
        sendReply(501, "Invalid REST parameter");
        return;
    }
    m_restOffset = v;
    sendReply(350, "Restarting at " + arg);
}

// ---- PASV + 数据通道命令 ----

void FtpSession::handlePasv() {
    if (!requireLoggedIn()) return;
    m_dataListener.close();
    if (!m_dataListener.bindAndListen(0)) { // 端口 0:操作系统挑一个空闲临时端口
        sendReply(425, "Cannot open passive connection");
        return;
    }
    const uint16_t port = m_dataListener.localPort();
    const std::string ip = m_control.localAddress(); // 客户端连过来的那个接口地址

    std::vector<int> octets;
    std::stringstream iss(ip);
    std::string tok;
    while (std::getline(iss, tok, '.')) octets.push_back(std::atoi(tok.c_str()));
    if (octets.size() != 4) {
        m_dataListener.close();
        sendReply(425, "Cannot determine local address");
        return;
    }

    const int p1 = (port >> 8) & 0xFF;
    const int p2 = port & 0xFF;
    std::ostringstream oss;
    oss << "Entering Passive Mode (" << octets[0] << "," << octets[1] << "," << octets[2] << "," << octets[3] << ","
        << p1 << "," << p2 << ").";
    sendReply(227, oss.str());
}

void FtpSession::handleList(const std::string& arg, bool mlsd) {
    if (!requireLoggedIn()) return;
    const std::string virtualPath = arg.empty() ? m_cwd : resolveVirtualPath(arg);
    const std::string osPath = normalizeFtpPath(m_rootPath, virtualPath);
    std::error_code ec;
    if (!fs::is_directory(osPath, ec)) {
        sendReply(550, "Not a directory");
        return;
    }

    Socket dataSocket;
    if (!acceptDataConnection(dataSocket, kDataAcceptTimeoutMs)) {
        sendReply(425, "Cannot open data connection");
        return;
    }
    sendReply(150, "Here comes the directory listing");

    std::vector<fs::directory_entry> entries;
    for (auto& e : fs::directory_iterator(osPath, ec)) entries.push_back(e);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        const bool ad = a.is_directory();
        const bool bd = b.is_directory();
        if (ad != bd) return ad && !bd;
        return a.path().filename().string() < b.path().filename().string();
    });

    std::ostringstream out;
    for (auto& e : entries) {
        std::error_code entryEc;
        const std::string name = e.path().filename().string();
        const bool isDir = e.is_directory(entryEc);
        const uint64_t size = isDir ? 0 : static_cast<uint64_t>(e.file_size(entryEc));
        if (mlsd) {
            out << "Type=" << (isDir ? "dir" : "file") << ";Size=" << size << "; " << name << "\r\n";
        } else {
            // 8 个空白分隔的字段 + 文件名,和 FtpClient::parseListLine 的 Unix
            // 解析逻辑(tokens[4] 是 size)对齐。
            out << (isDir ? "d" : "-") << "rwxr-xr-x 1 owner group " << size << " Jan 01 00:00 " << name << "\r\n";
        }
    }

    const std::string listing = out.str();
    const bool sent = dataSocket.sendAll(listing.data(), listing.size());
    dataSocket.close();

    if (!sent) {
        sendReply(426, "Connection closed; transfer aborted");
        return;
    }
    sendReply(226, "Directory send OK");
}

void FtpSession::handleRetr(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string osPath = normalizeFtpPath(m_rootPath, resolveVirtualPath(arg));
    const uint64_t offset = m_restOffset;
    m_restOffset = 0;

    std::error_code ec;
    if (!fs::is_regular_file(osPath, ec)) {
        sendReply(550, "File not found");
        return;
    }

    std::ifstream file(osPath, std::ios::binary);
    if (!file.is_open()) {
        sendReply(550, "Cannot open file");
        return;
    }
    file.seekg(static_cast<std::streamoff>(offset));

    Socket dataSocket;
    if (!acceptDataConnection(dataSocket, kDataAcceptTimeoutMs)) {
        sendReply(425, "Cannot open data connection");
        return;
    }
    sendReply(150, "Opening BINARY mode data connection for " + arg);

    // RETR 是服务端发送、大小提前已知(文件本身的大小),不像 STOR 那样 total 只能
    // 传 0——和 core::FtpClient 自己下载时上报的语义一致。
    const uint64_t totalSize = static_cast<uint64_t>(fs::file_size(osPath, ec));
    const std::string fileName = fs::path(osPath).filename().string();
    if (m_onTransferStarted) m_onTransferStarted(m_sessionId, fileName, /*isUpload=*/false);

    std::vector<char> chunk(kChunkSize);
    uint64_t transferred = offset;
    std::chrono::steady_clock::time_point lastReportTime{};
    bool failed = false;
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize n = file.gcount();
        if (n <= 0) break;
        if (!dataSocket.sendAll(chunk.data(), static_cast<size_t>(n))) {
            failed = true;
            break;
        }
        transferred += static_cast<uint64_t>(n);
        reportTransferProgress(transferred, totalSize, lastReportTime);
    }
    dataSocket.close();

    if (failed) {
        sendReply(426, "Connection closed; transfer aborted");
        return;
    }
    sendReply(226, "Transfer complete");
    if (m_onTransferCompleted) m_onTransferCompleted(m_sessionId, fileName);
}

void FtpSession::handleStor(const std::string& arg) {
    if (!requireLoggedIn()) return;
    const std::string osPath = normalizeFtpPath(m_rootPath, resolveVirtualPath(arg));
    const fs::path p(osPath);
    if (!isValidFtpName(p.filename().string())) {
        sendReply(553, "Invalid file name");
        m_restOffset = 0;
        return;
    }

    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    const uint64_t offset = m_restOffset;
    m_restOffset = 0;

    std::fstream file;
    if (offset > 0 && fs::exists(osPath, ec)) {
        file.open(osPath, std::ios::binary | std::ios::in | std::ios::out);
        if (file.is_open()) file.seekp(static_cast<std::streamoff>(offset));
    }
    if (!file.is_open()) {
        file.open(osPath, std::ios::binary | std::ios::out | std::ios::trunc);
    }
    if (!file.is_open()) {
        sendReply(550, "Cannot open file for writing");
        return;
    }

    Socket dataSocket;
    if (!acceptDataConnection(dataSocket, kDataAcceptTimeoutMs)) {
        sendReply(425, "Cannot open data connection");
        return;
    }
    sendReply(150, "Ready to receive " + arg);

    // STOR 是服务端接收:标准 FTP 没有让客户端预先声明文件总大小的机制,total 传
    // 0 表示"未知"——和 core::FtpClient 自己上传时上报进度的约定完全一致。
    const std::string fileName = p.filename().string();
    if (m_onTransferStarted) m_onTransferStarted(m_sessionId, fileName, /*isUpload=*/true);

    std::vector<char> chunk(kChunkSize);
    uint64_t transferred = offset;
    std::chrono::steady_clock::time_point lastReportTime{};
    bool failed = false;
    for (;;) {
        const WaitResult wr = dataSocket.waitReadable(kDataStallTimeoutMs);
        if (wr == WaitResult::Timeout || wr == WaitResult::Error) {
            failed = true;
            break;
        }
        const long n = dataSocket.recvSome(chunk.data(), chunk.size());
        if (n == 0) break; // 对端正常关闭数据连接:传输完成
        if (n < 0) {
            failed = true;
            break;
        }
        file.write(chunk.data(), n);
        if (!file) {
            failed = true;
            break;
        }
        transferred += static_cast<uint64_t>(n);
        reportTransferProgress(transferred, 0, lastReportTime);
    }
    file.close();
    dataSocket.close();

    if (failed) {
        sendReply(426, "Connection closed; transfer aborted");
        return;
    }
    sendReply(226, "Transfer complete");
    if (m_onTransferCompleted) m_onTransferCompleted(m_sessionId, fileName);
}

// ---- 主循环 ----

void FtpSession::run() {
    m_peerAddress = m_control.peerAddress();
    log("connected");
    sendReply(220, "LanDrop core FTP server ready");

    while (!m_stopRequested.load()) {
        std::string line;
        if (!readLine(line, kControlIdleTimeoutMs)) break;
        if (line.empty()) continue;

        std::string verb = line;
        std::string arg;
        const auto sp = line.find(' ');
        if (sp != std::string::npos) {
            verb = line.substr(0, sp);
            arg = line.substr(sp + 1);
        }
        std::transform(verb.begin(), verb.end(), verb.begin(), toUpperChar);

        if (verb == "USER") {
            handleUser(arg);
        } else if (verb == "PASS") {
            handlePass(arg);
        } else if (verb == "PWD" || verb == "XPWD") {
            handlePwd();
        } else if (verb == "CWD" || verb == "XCWD") {
            handleCwd(arg);
        } else if (verb == "CDUP" || verb == "XCUP") {
            handleCwd("..");
        } else if (verb == "TYPE") {
            handleType(arg);
        } else if (verb == "PASV") {
            handlePasv();
        } else if (verb == "LIST") {
            handleList(arg, false);
        } else if (verb == "MLSD") {
            handleList(arg, true);
        } else if (verb == "RETR") {
            handleRetr(arg);
        } else if (verb == "STOR") {
            handleStor(arg);
        } else if (verb == "REST") {
            handleRest(arg);
        } else if (verb == "MKD" || verb == "XMKD") {
            handleMkd(arg);
        } else if (verb == "RMD" || verb == "XRMD") {
            handleRmd(arg);
        } else if (verb == "DELE") {
            handleDele(arg);
        } else if (verb == "RNFR") {
            handleRnfr(arg);
        } else if (verb == "RNTO") {
            handleRnto(arg);
        } else if (verb == "NOOP") {
            sendReply(200, "NOOP ok");
        } else if (verb == "FEAT") {
            sendFeat();
        } else if (verb == "SYST") {
            sendReply(215, "UNIX Type: L8");
        } else if (verb == "QUIT") {
            sendReply(221, "Goodbye");
            break;
        } else {
            sendReply(502, "Command not implemented");
        }
    }

    m_control.close();
    m_dataListener.close();
    log("disconnected");
    m_finished = true;
}

} // namespace core
