// core::FtpClient 的回环集成测试:没有真实 FTP 服务器(Phase C 还没做),所以用
// core::Socket 直接实现一个按脚本应答的假 FTP 服务器,跑在独立线程里,验证真实
// FtpClient 通过真实 TCP 回环连接与它对话时,命令拼装/回复解析(含多行回复、
// PWD 引号提取、USER/PASS/TYPE/CWD/QUIT 状态机)是否正确——这是实际执行测试,
// 不是只看代码走查。
#include "../ftp/ftp_client.h"
#include "../platform/socket.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<int> g_failures{0};

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "[OK] " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

bool readLineFromSocket(core::Socket& sock, std::string& buffer, std::string& outLine, int timeoutMs) {
    for (;;) {
        const auto pos = buffer.find("\r\n");
        if (pos != std::string::npos) {
            outLine = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            return true;
        }
        if (sock.waitReadable(timeoutMs) != core::WaitResult::Ready) return false;
        char buf[4096];
        const long n = sock.recvSome(buf, sizeof(buf));
        if (n <= 0) return false;
        buffer.append(buf, static_cast<size_t>(n));
    }
}

void sendLine(core::Socket& sock, const std::string& line) {
    const std::string wire = line + "\r\n";
    sock.sendAll(wire.data(), wire.size());
}

void expectLine(core::Socket& sock, std::string& buffer, const std::string& expected, const std::string& what) {
    std::string line;
    const bool got = readLineFromSocket(sock, buffer, line, 5000);
    check(got && line == expected, what + " (expected '" + expected + "', got '" + (got ? line : "<timeout>") + "')");
}

void fakeServer(uint16_t port) {
    core::Socket server;
    if (!server.bindAndListen(port)) {
        std::cout << "[FAIL] fake server bindAndListen" << std::endl;
        ++g_failures;
        return;
    }
    core::Socket client;
    if (!server.accept(client, 5000)) {
        std::cout << "[FAIL] fake server accept timed out" << std::endl;
        ++g_failures;
        return;
    }

    std::string buffer;
    sendLine(client, "220 Fake FTP Ready");
    expectLine(client, buffer, "USER testuser", "server received USER");
    sendLine(client, "331 Password required");
    expectLine(client, buffer, "PASS testpass", "server received PASS");
    sendLine(client, "230 Logged in");
    expectLine(client, buffer, "TYPE I", "server received TYPE I");
    sendLine(client, "200 Type set to I");
    expectLine(client, buffer, "PWD", "server received PWD");
    sendLine(client, "257 \"/home/testuser\"");
    expectLine(client, buffer, "CWD /sub", "server received CWD");
    sendLine(client, "250 Directory changed");
    expectLine(client, buffer, "QUIT", "server received QUIT");
    sendLine(client, "221 Bye");
}

void testFullLoginFlow() {
    constexpr uint16_t kPort = 19560;
    std::thread serverThread(fakeServer, kPort);

    core::FtpClient ftp;
    check(ftp.connect("127.0.0.1", kPort, 3000) == core::FtpResult::Ok, "FtpClient::connect succeeds and reads 220 banner");
    check(ftp.login("testuser", "testpass") == core::FtpResult::Ok, "FtpClient::login (USER/PASS/TYPE I) succeeds");

    std::string path;
    check(ftp.pwd(path) == core::FtpResult::Ok, "FtpClient::pwd succeeds");
    check(path == "/home/testuser", "FtpClient::pwd extracts quoted path correctly (got '" + path + "')");

    check(ftp.cwd("/sub") == core::FtpResult::Ok, "FtpClient::cwd succeeds");

    ftp.disconnect(); // 触发 QUIT

    serverThread.join();
}

} // namespace

int main() {
    testFullLoginFlow();

    if (g_failures.load() == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures.load() << " FAILURE(S) ===" << std::endl;
    return 1;
}
