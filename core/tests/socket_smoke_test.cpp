// 阶段 A 验证程序:不链接 Qt,独立验证 core/ 里的 socket 封装、心跳线程、SHA-256
// 是否正确。用真实断言而不是"编译过就算数"——每一项都实际跑一遍并核对结果。
#include "../hash/sha256.h"
#include "../heartbeat.h"
#include "../platform/socket.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {
int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "[OK] " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

void testSocketLoopback() {
    core::Socket server;
    check(server.bindAndListen(19555), "server bindAndListen(19555)");

    std::string received;
    std::thread acceptThread([&] {
        core::Socket client;
        if (!server.accept(client, 5000)) {
            std::cout << "[FAIL] accept timed out" << std::endl;
            ++g_failures;
            return;
        }
        char buf[256] = {};
        const long n = client.recvSome(buf, sizeof(buf));
        if (n > 0) received.assign(buf, static_cast<size_t>(n));
    });

    core::Socket connector;
    const bool connected = connector.connectTo("127.0.0.1", 19555, 3000);
    check(connected, "client connectTo 127.0.0.1:19555");

    if (connected) {
        const std::string msg = "hello-core-socket";
        check(connector.sendAll(msg.data(), msg.size()), "client sendAll");
    }

    acceptThread.join();
    check(received == "hello-core-socket", "server received exact bytes sent (got: '" + received + "')");
}

void testWaitReadableTimeout() {
    core::Socket server;
    check(server.bindAndListen(19556), "server bindAndListen(19556) for timeout test");

    core::Socket client;
    check(client.connectTo("127.0.0.1", 19556, 3000), "client connectTo 19556");

    core::Socket accepted;
    check(server.accept(accepted, 3000), "server accept 19556");

    // 客户端不发数据,server 端等可读应该按超时返回,而不是无限期卡住。
    const auto start = std::chrono::steady_clock::now();
    const auto result = accepted.waitReadable(300);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    check(result == core::WaitResult::Timeout, "waitReadable times out when no data arrives");
    check(elapsedMs >= 280 && elapsedMs < 2000,
          "waitReadable timeout duration is roughly correct (~300ms, got " + std::to_string(elapsedMs) + "ms)");
}

void testHeartbeatFiresAndStops() {
    std::atomic<int> beatCount{0};
    core::Heartbeat hb(std::chrono::milliseconds(100), [&] {
        ++beatCount;
        return true;
    });
    hb.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    hb.stop();

    const int count = beatCount.load();
    check(count >= 2 && count <= 5,
          "heartbeat fired roughly 3 times in 350ms at 100ms interval (got " + std::to_string(count) + ")");

    const int afterStop = beatCount.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    check(beatCount.load() == afterStop, "heartbeat does not fire again after stop()");
}

void testHeartbeatFailureStopsSelfCleanly() {
    std::atomic<bool> failureCalled{false};
    core::Heartbeat hb(
        std::chrono::milliseconds(50), [] { return false; }, [&] { failureCalled = true; });
    hb.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(failureCalled.load(), "heartbeat onFailure invoked when heartbeatFn returns false");
    hb.stop(); // 必须能正常 join,不悬挂(如果这里卡住,进程会挂起,超时机制会暴露问题)
    check(true, "stop() after self-triggered failure returned cleanly");
}

void testSha256() {
    struct Vector {
        std::string input;
        std::string expectedHex;
    };
    // 用 `shasum -a 256` 现算的权威值,不是凭记忆手打的十六进制字符串。
    const Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"The quick brown fox jumps over the lazy dog",
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
        {std::string(1000, 'a'), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
    };

    for (const auto& v : vectors) {
        const auto digest = core::Sha256::hash(v.input.data(), v.input.size());
        const std::string hex = core::Sha256::toHex(digest);
        check(hex == v.expectedHex, "sha256(len=" + std::to_string(v.input.size()) + ") matches known vector");
    }

    // 增量 update() 应该和一次性 hash() 结果一致(验证多次 update 的缓冲区拼接逻辑)。
    core::Sha256 incremental;
    const std::string part1(30, 'x');
    const std::string part2(90, 'y'); // 跨越一个 64 字节块边界
    incremental.update(part1.data(), part1.size());
    incremental.update(part2.data(), part2.size());
    const auto incrementalDigest = incremental.finalize();

    const std::string whole = part1 + part2;
    const auto wholeDigest = core::Sha256::hash(whole.data(), whole.size());
    check(incrementalDigest == wholeDigest, "incremental update() matches one-shot hash() for the same bytes");
}

} // namespace

int main() {
    testSocketLoopback();
    testWaitReadableTimeout();
    testHeartbeatFiresAndStops();
    testHeartbeatFailureStopsSelfCleanly();
    testSha256();

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
