#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace core {

// 独立的心跳线程,协议无关——之后 FTP 客户端核心会传一个"在控制通道发 NOOP、
// 等回复"的 lambda 进来做 heartbeatFn。
//
// - 用 std::mutex + std::condition_variable 做"可中断的睡眠":stop() 会立刻
//   唤醒线程退出,不需要等到下一次 sleep_for 结束才反应。
// - heartbeatFn 返回 false 表示心跳失败(比如没在超时内收到期待的回复),
//   Heartbeat 会调用一次 onFailure 然后自行停止,不会无限重试——是否重连、
//   要不要重启心跳,由上层(FtpClient)决定。
class Heartbeat {
public:
    using HeartbeatFn = std::function<bool()>;
    using FailureFn = std::function<void()>;

    Heartbeat(std::chrono::milliseconds interval, HeartbeatFn heartbeatFn, FailureFn onFailure = nullptr);
    ~Heartbeat();

    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

    void start();
    // 阻塞直到心跳线程实际退出(join)。构造后即使从未 start() 也可以安全调用。
    void stop();
    bool isRunning() const { return m_running.load(); }

private:
    void run();

    std::chrono::milliseconds m_interval;
    HeartbeatFn m_heartbeatFn;
    FailureFn m_onFailure;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};

} // namespace core
