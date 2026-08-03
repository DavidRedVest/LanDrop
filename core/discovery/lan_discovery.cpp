#include "lan_discovery.h"

#include <algorithm>
#include <cstdlib>

namespace core {

namespace {
constexpr const char* kMagic = "LANDROP1";
constexpr int kListenPollTimeoutMs = 500; // 兼职当"多久检查一次过期设备"的节拍

bool parsePacket(const std::string& raw, const std::string& fromAddress, DiscoveredDevice& out) {
    const auto firstPipe = raw.find('|');
    const auto lastPipe = raw.rfind('|');
    if (firstPipe == std::string::npos || lastPipe == firstPipe) return false;
    if (raw.compare(0, firstPipe, kMagic) != 0) return false; // 不是我们自己的广播包,忽略

    const std::string name = raw.substr(firstPipe + 1, lastPipe - firstPipe - 1);
    const std::string portStr = raw.substr(lastPipe + 1);
    if (name.empty() || portStr.empty()) return false;

    char* end = nullptr;
    const long port = std::strtol(portStr.c_str(), &end, 10);
    if (end == portStr.c_str() || port <= 0 || port > 65535) return false;

    out.deviceName = name;
    out.address = fromAddress;
    out.servicePort = static_cast<uint16_t>(port);
    out.lastSeen = std::chrono::steady_clock::now();
    return true;
}
} // namespace

// ---- DiscoveryBeacon ----

DiscoveryBeacon::DiscoveryBeacon(std::string deviceName, uint16_t servicePort, uint16_t discoveryPort,
                                  std::chrono::milliseconds interval)
    : m_deviceName(std::move(deviceName)), m_servicePort(servicePort), m_discoveryPort(discoveryPort),
      m_interval(interval) {}

DiscoveryBeacon::~DiscoveryBeacon() {
    stop();
}

std::string DiscoveryBeacon::buildPacket() const {
    return std::string(kMagic) + "|" + m_deviceName + "|" + std::to_string(m_servicePort);
}

void DiscoveryBeacon::start() {
    if (m_running.exchange(true)) return;
    m_stopRequested = false;
    m_socket.setBroadcast(true);
    m_thread = std::thread(&DiscoveryBeacon::run, this);
}

void DiscoveryBeacon::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void DiscoveryBeacon::run() {
    const std::string packet = buildPacket();
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopRequested) {
        lock.unlock();
        m_socket.sendTo("255.255.255.255", m_discoveryPort, packet.data(), packet.size());
        lock.lock();
        // 和 Heartbeat::run() 一样用 wait_for + predicate 做可中断睡眠:stop() 能
        // 立刻唤醒线程退出,不用等到下一次广播间隔结束。第一次广播在进入循环时
        // 立即发出(不是先等一个 interval),新起的服务尽快能被发现。
        const bool wokenByStop = m_cv.wait_for(lock, m_interval, [this] { return m_stopRequested.load(); });
        if (wokenByStop) break;
    }
}

// ---- DiscoveryListener ----

DiscoveryListener::DiscoveryListener(uint16_t discoveryPort, std::chrono::milliseconds staleTimeout)
    : m_discoveryPort(discoveryPort), m_staleTimeout(staleTimeout) {}

DiscoveryListener::~DiscoveryListener() {
    stop();
}

bool DiscoveryListener::start() {
    if (m_running.exchange(true)) return true;
    if (!m_socket.bind(m_discoveryPort)) {
        m_running = false;
        return false;
    }
    m_stopRequested = false;
    m_thread = std::thread(&DiscoveryListener::run, this);
    return true;
}

void DiscoveryListener::stop() {
    if (!m_running.exchange(false)) return;
    m_stopRequested = true;
    m_socket.close(); // 尽快唤醒可能卡在 recvFrom() 里的线程,不用等下一次轮询超时
    if (m_thread.joinable()) m_thread.join();
}

void DiscoveryListener::run() {
    std::vector<char> buf(1024);
    while (!m_stopRequested.load()) {
        std::string fromAddress;
        uint16_t fromPort = 0;
        const long n = m_socket.recvFrom(buf.data(), buf.size(), fromAddress, fromPort, kListenPollTimeoutMs);
        if (n > 0) {
            handlePacket(std::string(buf.data(), static_cast<size_t>(n)), fromAddress);
        }
        // 不管这一轮有没有收到包都检查一次过期——这个 500ms 轮询本身就是"多久
        // 检查一次"的节拍,和 FtpServer::acceptLoop() 里 500ms 轮询检查 m_running
        // 是同一个思路。
        pruneStale();
    }
}

void DiscoveryListener::handlePacket(const std::string& raw, const std::string& fromAddress) {
    DiscoveredDevice parsed;
    if (!parsePacket(raw, fromAddress, parsed)) return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        auto it = std::find_if(m_devices.begin(), m_devices.end(), [&](const DiscoveredDevice& d) {
            return d.address == parsed.address && d.servicePort == parsed.servicePort;
        });
        if (it == m_devices.end()) {
            m_devices.push_back(parsed);
            changed = true;
        } else {
            // lastSeen 更新本身不算"列表变化"——否则每隔一个广播周期就会触发一次
            // 回调,又是一次不必要的高频通知;只有设备名变了才需要通知 UI 刷新。
            if (it->deviceName != parsed.deviceName) changed = true;
            it->deviceName = parsed.deviceName;
            it->lastSeen = parsed.lastSeen;
        }
    }
    if (changed) fireUpdate();
}

void DiscoveryListener::pruneStale() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        const auto now = std::chrono::steady_clock::now();
        const size_t before = m_devices.size();
        m_devices.erase(std::remove_if(m_devices.begin(), m_devices.end(),
                                        [&](const DiscoveredDevice& d) { return now - d.lastSeen > m_staleTimeout; }),
                         m_devices.end());
        changed = m_devices.size() != before;
    }
    if (changed) fireUpdate();
}

void DiscoveryListener::fireUpdate() {
    if (m_onUpdate) m_onUpdate(snapshot());
}

std::vector<DiscoveredDevice> DiscoveryListener::snapshot() const {
    std::lock_guard<std::mutex> lock(m_devicesMutex);
    return m_devices;
}

} // namespace core
