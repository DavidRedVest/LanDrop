#pragma once

// 局域网自动发现:自定义 UDP 广播协议,不是 mDNS/DNS-SD——目标只是"LanDrop 客户端
// 发现 LanDrop 服务端",不需要被系统级 Bonjour/Avahi 这类工具识别,手写一个标准
// mDNS 子集的代价(DNS 报文编解码)相对这个实际需求不成比例。不依赖 Qt。
//
// 协议:一行文本 "LANDROP1|<设备名>|<FTP 服务端口>",服务端(DiscoveryBeacon)
// 周期性广播到 255.255.255.255:kDefaultDiscoveryPort;客户端(DiscoveryListener)
// 监听同一端口,维护一张按"来源 IP + 服务端口"去重的"发现的设备"表,记录每个
// 设备最后一次收到广播的时间,超过 staleTimeout 没再收到就自动从表里移除。

#include "../platform/udp_socket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

// 默认发现端口,故意跟 FTP 默认控制端口(2121)不一样,避免和真正的服务混淆。
constexpr uint16_t kDefaultDiscoveryPort = 52121;

struct DiscoveredDevice {
    std::string deviceName;
    std::string address;    // 广播包发送方的源 IP(不是设备自己上报的,更可信)
    uint16_t servicePort = 0; // FTP 服务监听端口(不是这个发现协议自己用的 UDP 端口)
    std::chrono::steady_clock::time_point lastSeen;
};

using DiscoveryUpdateCallback = std::function<void(const std::vector<DiscoveredDevice>&)>;

// 服务端用:周期广播自己的存在。start()/stop() 语义和 core::Heartbeat 一致
// (可中断等待、干净停止),内部只有一条发送线程,不需要收包。
class DiscoveryBeacon {
public:
    DiscoveryBeacon(std::string deviceName, uint16_t servicePort, uint16_t discoveryPort = kDefaultDiscoveryPort,
                     std::chrono::milliseconds interval = std::chrono::milliseconds(3000));
    ~DiscoveryBeacon();

    DiscoveryBeacon(const DiscoveryBeacon&) = delete;
    DiscoveryBeacon& operator=(const DiscoveryBeacon&) = delete;

    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }

private:
    void run();
    std::string buildPacket() const;

    std::string m_deviceName;
    uint16_t m_servicePort;
    uint16_t m_discoveryPort;
    std::chrono::milliseconds m_interval;

    UdpSocket m_socket;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};

// 客户端用:持续监听广播,维护"发现的设备"列表。回调只在列表实际发生变化时
// (新增/移除/改名)触发,不是每收到一个广播包就触发一次——高频触发一个之后会被
// Qt 侧跨线程排队处理的回调,是这个项目已经真实踩过的坑(见 client/transfer.cpp
// 里 onWorkerProgress() 的节流注释),这里从设计上直接避免。
class DiscoveryListener {
public:
    explicit DiscoveryListener(uint16_t discoveryPort = kDefaultDiscoveryPort,
                                std::chrono::milliseconds staleTimeout = std::chrono::milliseconds(10000));
    ~DiscoveryListener();

    DiscoveryListener(const DiscoveryListener&) = delete;
    DiscoveryListener& operator=(const DiscoveryListener&) = delete;

    void setUpdateCallback(DiscoveryUpdateCallback cb) { m_onUpdate = std::move(cb); }

    bool start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    // 当前发现的设备快照,按最近一次内部更新的顺序——供 Qt 层不依赖回调、随时
    // 主动查询一次当前状态(比如刚打开发现面板时先拿一次已有的结果)。
    std::vector<DiscoveredDevice> snapshot() const;

private:
    void run();
    void handlePacket(const std::string& raw, const std::string& fromAddress);
    void pruneStale();
    void fireUpdate();

    uint16_t m_discoveryPort;
    std::chrono::milliseconds m_staleTimeout;

    UdpSocket m_socket;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    mutable std::mutex m_devicesMutex;
    std::vector<DiscoveredDevice> m_devices;

    DiscoveryUpdateCallback m_onUpdate;
};

} // namespace core
