#include "heartbeat.h"

namespace core {

Heartbeat::Heartbeat(std::chrono::milliseconds interval, HeartbeatFn heartbeatFn, FailureFn onFailure)
    : m_interval(interval), m_heartbeatFn(std::move(heartbeatFn)), m_onFailure(std::move(onFailure)) {}

Heartbeat::~Heartbeat() {
    stop();
}

void Heartbeat::start() {
    if (m_running.exchange(true)) return; // 已经在跑,忽略重复 start()
    m_stopRequested = false;
    m_thread = std::thread(&Heartbeat::run, this);
}

void Heartbeat::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running = false;
}

void Heartbeat::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopRequested) {
        // wait_for 在超时或被 notify 唤醒时都会返回；用 predicate 版本可以正确处理
        // "假唤醒"，并且一旦 m_stopRequested 变 true 会立刻返回 true 提前退出等待。
        const bool wokenByStop = m_cv.wait_for(lock, m_interval, [this] { return m_stopRequested.load(); });
        if (wokenByStop) break;

        // 调用 heartbeatFn 时释放锁——它通常会做网络 I/O，不能让 stop() 卡在这里等锁。
        lock.unlock();
        const bool ok = m_heartbeatFn ? m_heartbeatFn() : true;
        lock.lock();

        if (!ok) {
            m_stopRequested = true; // 心跳失败：自行停止，不再重试，由上层决定要不要重连
            lock.unlock();
            if (m_onFailure) m_onFailure();
            lock.lock();
            break;
        }
    }
}

} // namespace core
