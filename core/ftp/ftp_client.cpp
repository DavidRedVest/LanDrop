#include "ftp_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {

namespace {
// 64KB-1MB 区间内选一个折中值,与旧自定义协议里验证过的 BLOCK_SIZE 一致。
constexpr size_t kChunkSize = 262144;
constexpr int kHeartbeatIntervalMs = 45000; // 30-60s 区间内。
constexpr int kDataChannelStallTimeoutMs = 20000;
} // namespace

FtpClient::FtpClient() = default;

FtpClient::~FtpClient() {
    disconnect();
}

// ---- 连接/登录/断开 ----

FtpResult FtpClient::connect(const std::string& host, uint16_t port, int timeoutMs) {
    disconnect();
    m_recvBuffer.clear();
    setState(TransferState::Connecting);

    if (!m_control.connectTo(host, port, timeoutMs)) {
        reportError("connect to " + host + ":" + std::to_string(port) + " failed");
        setState(TransferState::Failed);
        return FtpResult::ConnectFailed;
    }

    int code = 0;
    std::string message;
    if (!readReply(code, message, timeoutMs) || code != 220) {
        reportError("no welcome banner from server: " + message);
        m_control.close();
        setState(TransferState::Failed);
        return FtpResult::ConnectFailed;
    }

    log("connected: " + message);
    setState(TransferState::Idle);
    return FtpResult::Ok;
}

FtpResult FtpClient::login(const std::string& username, const std::string& password) {
    if (!m_control.isValid()) return FtpResult::NotConnected;

    int code = 0;
    std::string message;
    if (!command("USER " + username, code, message)) {
        reportError("USER command failed to send");
        return FtpResult::LoginFailed;
    }
    if (code == 331) {
        if (!command("PASS " + password, code, message) || code != 230) {
            reportError("login failed: " + message);
            return FtpResult::LoginFailed;
        }
    } else if (code != 230) {
        reportError("USER rejected: " + message);
        return FtpResult::LoginFailed;
    }

    // 固定用二进制模式,避免 ASCII 模式对非文本文件做换行转换损坏数据。
    if (!command("TYPE I", code, message) || code != 200) {
        reportError("TYPE I failed: " + message);
        return FtpResult::LoginFailed;
    }

    m_loggedIn = true;
    m_featChecked = false;
    startHeartbeat();
    setState(TransferState::Idle);
    return FtpResult::Ok;
}

void FtpClient::disconnect() {
    if (m_heartbeat) {
        m_heartbeat->stop();
        m_heartbeat.reset();
    }
    if (m_control.isValid()) {
        int code = 0;
        std::string message;
        command("QUIT", code, message, 3000); // 尽力而为,不关心结果
        m_control.close();
    }
    m_loggedIn = false;
    setState(TransferState::Idle);
}

// ---- 心跳:仅在控制通道空闲(没有传输在进行)时发 NOOP ----

void FtpClient::startHeartbeat() {
    m_heartbeat.reset(new Heartbeat(
        std::chrono::milliseconds(kHeartbeatIntervalMs), [this] { return sendHeartbeatNoop(); },
        [this] { reportError("control channel heartbeat failed, connection may be dead"); }));
    m_heartbeat->start();
}

bool FtpClient::sendHeartbeatNoop() {
    // 传输期间控制通道的“命令槽”被 RETR/STOR 占着(在等最终 226 响应),这时候
    // 插进一个 NOOP 会和真正的响应交错、读出乱码——跳过这一拍,不发送。传输本身
    // 会让数据通道保持忙碌,不会被 NAT/防火墙判定为空闲;真正需要心跳保护的是
    // “已连接但用户没有传输动作”的浏览/空闲窗口。
    if (m_transferActive.load()) return true;
    if (!m_control.isValid()) return false;
    int code = 0;
    std::string message;
    if (!command("NOOP", code, message, 10000)) return false;
    return code == 200;
}

void FtpClient::beginTransfer() {
    m_transferActive = true;
    m_cancelRequested = false;
    setState(TransferState::Transferring);
}

void FtpClient::endTransfer() {
    m_transferActive = false;
}

// ---- 底层控制通道读写 ----

bool FtpClient::readLine(std::string& outLine, int timeoutMs) {
    for (;;) {
        const auto pos = m_recvBuffer.find("\r\n");
        if (pos != std::string::npos) {
            outLine = m_recvBuffer.substr(0, pos);
            m_recvBuffer.erase(0, pos + 2);
            return true;
        }
        if (m_control.waitReadable(timeoutMs) != WaitResult::Ready) return false;
        char buf[4096];
        const long n = m_control.recvSome(buf, sizeof(buf));
        if (n <= 0) return false;
        m_recvBuffer.append(buf, static_cast<size_t>(n));
    }
}

bool FtpClient::readReply(int& outCode, std::string& outMessage, int timeoutMs) {
    std::string line;
    if (!readLine(line, timeoutMs)) return false;
    if (line.size() < 4) return false;
    outCode = std::atoi(line.substr(0, 3).c_str());

    if (line[3] != '-') {
        outMessage = line.substr(4);
        return true;
    }

    // 多行回复(RFC 959 4.2):第一行 "code-text",中间行任意,最后一行必须是
    // "code text"(空格,不是连字符)才算结束。
    const std::string finalPrefix = line.substr(0, 3) + " ";
    outMessage = line.substr(4);
    for (;;) {
        std::string next;
        if (!readLine(next, timeoutMs)) return false;
        if (next.compare(0, finalPrefix.size(), finalPrefix) == 0) {
            outMessage += "\n" + next.substr(finalPrefix.size());
            return true;
        }
        outMessage += "\n" + next;
    }
}

bool FtpClient::command(const std::string& line, int& outCode, std::string& outMessage, int timeoutMs) {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    const std::string wire = line + "\r\n";
    if (!m_control.sendAll(wire.data(), wire.size())) return false;
    return readReply(outCode, outMessage, timeoutMs);
}

// ---- 简单命令 ----

FtpResult FtpClient::pwd(std::string& outPath) {
    int code = 0;
    std::string message;
    if (!command("PWD", code, message) || code != 257) {
        reportError("PWD failed: " + message);
        return FtpResult::CommandFailed;
    }
    const auto first = message.find('"');
    const auto last = message.rfind('"');
    if (first == std::string::npos || last == std::string::npos || last <= first) {
        reportError("PWD reply did not contain a quoted path: " + message);
        return FtpResult::CommandFailed;
    }
    outPath = message.substr(first + 1, last - first - 1);
    return FtpResult::Ok;
}

FtpResult FtpClient::cwd(const std::string& path) {
    int code = 0;
    std::string message;
    if (!command("CWD " + path, code, message) || code != 250) {
        reportError("CWD failed: " + message);
        return FtpResult::CommandFailed;
    }
    return FtpResult::Ok;
}

FtpResult FtpClient::mkdir(const std::string& path) {
    int code = 0;
    std::string message;
    if (!command("MKD " + path, code, message) || code != 257) {
        reportError("MKD failed: " + message);
        return FtpResult::CommandFailed;
    }
    return FtpResult::Ok;
}

FtpResult FtpClient::rmdir(const std::string& path) {
    int code = 0;
    std::string message;
    if (!command("RMD " + path, code, message) || code != 250) {
        reportError("RMD failed: " + message);
        return FtpResult::CommandFailed;
    }
    return FtpResult::Ok;
}

FtpResult FtpClient::removeFile(const std::string& path) {
    int code = 0;
    std::string message;
    if (!command("DELE " + path, code, message) || code != 250) {
        reportError("DELE failed: " + message);
        return FtpResult::CommandFailed;
    }
    return FtpResult::Ok;
}

FtpResult FtpClient::rename(const std::string& fromPath, const std::string& toPath) {
    int code = 0;
    std::string message;
    if (!command("RNFR " + fromPath, code, message) || code != 350) {
        reportError("RNFR rejected: " + message);
        return FtpResult::CommandFailed;
    }
    if (!command("RNTO " + toPath, code, message) || code != 250) {
        reportError("RNTO rejected: " + message);
        return FtpResult::CommandFailed;
    }
    return FtpResult::Ok;
}

// ---- 目录列表 ----

void FtpClient::detectMlsdSupport() {
    m_featChecked = true;
    int code = 0;
    std::string message;
    if (!command("FEAT", code, message, 10000) || code != 211) {
        m_mlsdSupported = false;
        return;
    }
    std::string upper = message;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    m_mlsdSupported = upper.find("MLSD") != std::string::npos;
}

bool FtpClient::enterPassiveMode(std::string& outIp, uint16_t& outPort) {
    int code = 0;
    std::string message;
    if (!command("PASV", code, message, 10000) || code != 227) return false;
    return parsePasvReply(message, outIp, outPort);
}

bool FtpClient::parsePasvReply(const std::string& message, std::string& outIp, uint16_t& outPort) {
    const auto open = message.find('(');
    const auto close = message.find(')', open == std::string::npos ? 0 : open);
    std::string nums;
    if (open != std::string::npos && close != std::string::npos && close > open) {
        nums = message.substr(open + 1, close - open - 1);
    } else {
        for (char c : message) {
            if (std::isdigit(static_cast<unsigned char>(c)) || c == ',') nums += c;
        }
    }

    int parts[6] = {0, 0, 0, 0, 0, 0};
    std::istringstream iss(nums);
    std::string token;
    int idx = 0;
    while (idx < 6 && std::getline(iss, token, ',')) {
        parts[idx++] = std::atoi(token.c_str());
    }
    if (idx != 6) return false;

    outIp = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." +
            std::to_string(parts[3]);
    outPort = static_cast<uint16_t>(((parts[4] & 0xFF) << 8) | (parts[5] & 0xFF));
    return true;
}

void FtpClient::parseListLine(const std::string& rawLine, bool mlsd, std::vector<FtpFileEntry>& outEntries) {
    if (mlsd) {
        const auto lastSemi = rawLine.rfind(';');
        if (lastSemi == std::string::npos) return;
        const std::string facts = rawLine.substr(0, lastSemi + 1);
        std::string name = rawLine.substr(lastSemi + 1);
        if (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (name.empty() || name == "." || name == "..") return;

        FtpFileEntry entry;
        entry.name = name;

        size_t pos = 0;
        while (pos < facts.size()) {
            const auto semi = facts.find(';', pos);
            if (semi == std::string::npos) break;
            const std::string fact = facts.substr(pos, semi - pos);
            pos = semi + 1;
            const auto eq = fact.find('=');
            if (eq == std::string::npos) continue;
            std::string key = fact.substr(0, eq);
            const std::string value = fact.substr(eq + 1);
            std::transform(key.begin(), key.end(), key.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == "type") {
                entry.isDirectory = (value == "dir" || value == "cdir" || value == "pdir");
            } else if (key == "size") {
                entry.size = std::strtoull(value.c_str(), nullptr, 10);
            }
        }
        outEntries.push_back(entry);
        return;
    }

    // LIST(类 Unix ls -l)最佳努力解析——没有严格标准。
    if (rawLine.empty() || (rawLine[0] != 'd' && rawLine[0] != '-' && rawLine[0] != 'l')) return;

    std::istringstream iss(rawLine);
    std::vector<std::string> tokens;
    std::string tok;
    while (tokens.size() < 8 && (iss >> tok)) tokens.push_back(tok);
    if (tokens.size() < 8) return;

    std::string rest;
    std::getline(iss, rest);
    if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
    if (rest.empty() || rest == "." || rest == "..") return;

    FtpFileEntry entry;
    entry.isDirectory = (tokens[0][0] == 'd');
    entry.size = std::strtoull(tokens[4].c_str(), nullptr, 10);
    if (tokens[0][0] == 'l') {
        const auto arrow = rest.find(" -> ");
        if (arrow != std::string::npos) rest = rest.substr(0, arrow);
    }
    entry.name = rest;
    outEntries.push_back(entry);
}

FtpResult FtpClient::list(const std::string& path, std::vector<FtpFileEntry>& outEntries) {
    outEntries.clear();
    if (!isConnected() || !m_loggedIn) return FtpResult::NotConnected;
    if (!m_featChecked) detectMlsdSupport();

    std::string ip;
    uint16_t port = 0;
    if (!enterPassiveMode(ip, port)) {
        reportError("PASV failed");
        return FtpResult::DataChannelFailed;
    }

    Socket dataSocket;
    if (!dataSocket.connectTo(ip, port, 8000)) {
        reportError("data channel connect failed for LIST/MLSD");
        return FtpResult::DataChannelFailed;
    }

    const std::string cmdLine = (m_mlsdSupported ? "MLSD " : "LIST ") + path;
    int code = 0;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        const std::string wire = cmdLine + "\r\n";
        if (!m_control.sendAll(wire.data(), wire.size()) || !readReply(code, message, 15000) ||
            (code != 150 && code != 125)) {
            reportError("LIST/MLSD rejected: " + message);
            return FtpResult::CommandFailed;
        }
    }

    std::string listing;
    char buf[4096];
    for (;;) {
        const WaitResult wr = dataSocket.waitReadable(kDataChannelStallTimeoutMs);
        if (wr == WaitResult::Timeout) {
            dataSocket.close();
            reportError("LIST/MLSD data channel stalled (timeout)");
            return FtpResult::Timeout;
        }
        if (wr == WaitResult::Error) break;
        const long n = dataSocket.recvSome(buf, sizeof(buf));
        if (n <= 0) break;
        listing.append(buf, static_cast<size_t>(n));
    }
    dataSocket.close();

    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        readReply(code, message, 15000); // 226 收尾;这里失败不影响已经读到的列表内容
    }

    size_t start = 0;
    while (start < listing.size()) {
        const auto end = listing.find('\n', start);
        std::string line = (end == std::string::npos) ? listing.substr(start) : listing.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) parseListLine(line, m_mlsdSupported, outEntries);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return FtpResult::Ok;
}

// ---- 文件传输 ----

FtpResult FtpClient::downloadFile(const std::string& remotePath, const std::string& localPath, uint64_t resumeOffset) {
    if (!isConnected() || !m_loggedIn) return FtpResult::NotConnected;

    uint64_t effectiveOffset = resumeOffset;
    std::fstream file;
    if (effectiveOffset > 0) {
        file.open(localPath, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open()) effectiveOffset = 0; // 本地文件不存在,只能从头下载
    }
    if (!file.is_open()) {
        file.open(localPath, std::ios::binary | std::ios::out | std::ios::trunc);
    }
    if (!file.is_open()) {
        reportError("cannot open local file for write: " + localPath);
        return FtpResult::IoError;
    }
    if (effectiveOffset > 0) file.seekp(static_cast<std::streamoff>(effectiveOffset));

    beginTransfer();
    struct TransferGuard {
        FtpClient* self;
        ~TransferGuard() { self->endTransfer(); }
    } guard{this};

    std::string ip;
    uint16_t port = 0;
    if (!enterPassiveMode(ip, port)) {
        reportError("PASV failed");
        setState(TransferState::Failed);
        return FtpResult::DataChannelFailed;
    }

    Socket dataSocket;
    if (!dataSocket.connectTo(ip, port, 8000)) {
        reportError("data channel connect failed");
        setState(TransferState::Failed);
        return FtpResult::DataChannelFailed;
    }

    int code = 0;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (effectiveOffset > 0) {
            const std::string restWire = "REST " + std::to_string(effectiveOffset) + "\r\n";
            if (!m_control.sendAll(restWire.data(), restWire.size()) || !readReply(code, message) || code != 350) {
                reportError("REST rejected: " + message);
                setState(TransferState::Failed);
                return FtpResult::CommandFailed;
            }
        }
        const std::string wire = "RETR " + remotePath + "\r\n";
        if (!m_control.sendAll(wire.data(), wire.size()) || !readReply(code, message, 15000) ||
            (code != 150 && code != 125)) {
            reportError("RETR rejected: " + message);
            setState(TransferState::Failed);
            return FtpResult::CommandFailed;
        }
    }

    uint64_t transferred = effectiveOffset;
    std::vector<char> chunk(kChunkSize);
    for (;;) {
        if (m_cancelRequested.load()) {
            dataSocket.close();
            std::lock_guard<std::mutex> lock(m_controlMutex);
            const std::string abor = "ABOR\r\n";
            m_control.sendAll(abor.data(), abor.size());
            readReply(code, message, 3000);
            setState(TransferState::Cancelled);
            return FtpResult::Cancelled;
        }

        const WaitResult wr = dataSocket.waitReadable(kDataChannelStallTimeoutMs);
        if (wr == WaitResult::Timeout) {
            reportError("data channel stalled (timeout)");
            setState(TransferState::Failed);
            return FtpResult::Timeout;
        }
        if (wr == WaitResult::Error) {
            reportError("data channel error");
            setState(TransferState::Failed);
            return FtpResult::DataChannelFailed;
        }

        const long n = dataSocket.recvSome(chunk.data(), chunk.size());
        if (n == 0) break; // 服务端正常关闭数据连接:传输完成
        if (n < 0) {
            reportError("data channel read error");
            setState(TransferState::Failed);
            return FtpResult::DataChannelFailed;
        }

        file.write(chunk.data(), n);
        if (!file) {
            reportError("local write failed: " + localPath);
            setState(TransferState::Failed);
            return FtpResult::IoError;
        }
        transferred += static_cast<uint64_t>(n);
        if (m_onProgress) m_onProgress(transferred, 0);
    }
    file.close();
    dataSocket.close();

    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (!readReply(code, message, 15000) || (code != 226 && code != 250)) {
            reportError("transfer did not complete cleanly: " + message);
            setState(TransferState::Failed);
            return FtpResult::CommandFailed;
        }
    }

    setState(TransferState::Completed);
    return FtpResult::Ok;
}

FtpResult FtpClient::uploadFile(const std::string& localPath, const std::string& remotePath, uint64_t resumeOffset) {
    if (!isConnected() || !m_loggedIn) return FtpResult::NotConnected;

    std::ifstream file(localPath, std::ios::binary);
    if (!file.is_open()) {
        reportError("cannot open local file for read: " + localPath);
        return FtpResult::IoError;
    }
    file.seekg(static_cast<std::streamoff>(resumeOffset));

    beginTransfer();
    struct TransferGuard {
        FtpClient* self;
        ~TransferGuard() { self->endTransfer(); }
    } guard{this};

    std::string ip;
    uint16_t port = 0;
    if (!enterPassiveMode(ip, port)) {
        reportError("PASV failed");
        setState(TransferState::Failed);
        return FtpResult::DataChannelFailed;
    }

    Socket dataSocket;
    if (!dataSocket.connectTo(ip, port, 8000)) {
        reportError("data channel connect failed");
        setState(TransferState::Failed);
        return FtpResult::DataChannelFailed;
    }

    int code = 0;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (resumeOffset > 0) {
            const std::string restWire = "REST " + std::to_string(resumeOffset) + "\r\n";
            if (!m_control.sendAll(restWire.data(), restWire.size()) || !readReply(code, message) || code != 350) {
                reportError("REST rejected: " + message);
                setState(TransferState::Failed);
                return FtpResult::CommandFailed;
            }
        }
        const std::string wire = "STOR " + remotePath + "\r\n";
        if (!m_control.sendAll(wire.data(), wire.size()) || !readReply(code, message, 15000) ||
            (code != 150 && code != 125)) {
            reportError("STOR rejected: " + message);
            setState(TransferState::Failed);
            return FtpResult::CommandFailed;
        }
    }

    // 用非阻塞 + sendSome()/waitWritable() 自己做分块发送循环,而不是 Socket::sendAll——
    // sendAll 在写缓冲区满时内部会用 timeoutMs=-1 无限等待,没法在这里被取消标志中断。
    dataSocket.setNonBlocking(true);
    uint64_t transferred = resumeOffset;
    std::vector<char> chunk(kChunkSize);
    bool failed = false;
    bool cancelled = false;

    while (file) {
        if (m_cancelRequested.load()) {
            cancelled = true;
            break;
        }
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize n = file.gcount();
        if (n <= 0) break;

        size_t off = 0;
        while (off < static_cast<size_t>(n)) {
            if (m_cancelRequested.load()) {
                cancelled = true;
                break;
            }
            const long sent = dataSocket.sendSome(chunk.data() + off, static_cast<size_t>(n) - off);
            if (sent == -2) {
                if (dataSocket.waitWritable(kDataChannelStallTimeoutMs) != WaitResult::Ready) {
                    reportError("data channel stalled (timeout)");
                    failed = true;
                    break;
                }
                continue;
            }
            if (sent <= 0) {
                reportError("data channel write error");
                failed = true;
                break;
            }
            off += static_cast<size_t>(sent);
            transferred += static_cast<uint64_t>(sent);
            if (m_onProgress) m_onProgress(transferred, 0);
        }
        if (failed || cancelled) break;
    }
    dataSocket.close(); // 主动关闭数据连接,向服务端发出 EOF 信号

    if (cancelled) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        const std::string abor = "ABOR\r\n";
        m_control.sendAll(abor.data(), abor.size());
        readReply(code, message, 3000);
        setState(TransferState::Cancelled);
        return FtpResult::Cancelled;
    }
    if (failed) {
        setState(TransferState::Failed);
        std::lock_guard<std::mutex> lock(m_controlMutex);
        readReply(code, message, 5000); // 尽量把服务端响应读掉,避免下一条命令读到脏数据
        return FtpResult::DataChannelFailed;
    }

    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (!readReply(code, message, 15000) || (code != 226 && code != 250)) {
            reportError("transfer did not complete cleanly: " + message);
            setState(TransferState::Failed);
            return FtpResult::CommandFailed;
        }
    }

    setState(TransferState::Completed);
    return FtpResult::Ok;
}

// ---- 回调转发 ----

void FtpClient::setState(TransferState state) {
    if (m_onState) m_onState(state);
}

void FtpClient::reportError(const std::string& message) {
    if (m_onError) m_onError(message);
}

void FtpClient::log(const std::string& message) {
    if (m_onLog) m_onLog(message);
}

} // namespace core
