// core::DiscoveryBeacon / core::DiscoveryListener 的真实集成测试:一个真实
// DiscoveryBeacon 在回环网络上广播,一个真实 DiscoveryListener 监听并发现它——
// 实际跑一遍,不是只看代码走查。覆盖:能发现、多次广播不会重复触发变化回调、
// 停止广播后设备会因为超时被自动移除。
#include "../discovery/lan_discovery.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
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

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

} // namespace

int main() {
    constexpr uint16_t kDiscoveryPort = 52122; // 避开默认端口,防止和本机其它测试/真实实例冲突

    std::mutex updatesMutex;
    int updateCount = 0;
    std::vector<core::DiscoveredDevice> lastSnapshot;

    core::DiscoveryListener listener(kDiscoveryPort, /*staleTimeout=*/std::chrono::milliseconds(800));
    listener.setUpdateCallback([&](const std::vector<core::DiscoveredDevice>& devices) {
        std::lock_guard<std::mutex> lock(updatesMutex);
        ++updateCount;
        lastSnapshot = devices;
    });
    check(listener.start(), "DiscoveryListener::start binds the UDP discovery port");

    {
        core::DiscoveryBeacon beacon("测试设备A", /*servicePort=*/2121, kDiscoveryPort,
                                      /*interval=*/std::chrono::milliseconds(150));
        beacon.start();

        check(waitUntil(
                  [&] {
                      std::lock_guard<std::mutex> lock(updatesMutex);
                      return !lastSnapshot.empty();
                  },
                  3000),
              "listener discovers the beacon's device within a few broadcast intervals");

        {
            std::lock_guard<std::mutex> lock(updatesMutex);
            check(lastSnapshot.size() == 1, "exactly one device discovered (got " +
                                                 std::to_string(lastSnapshot.size()) + ")");
            if (!lastSnapshot.empty()) {
                check(lastSnapshot[0].deviceName == "测试设备A",
                      "discovered device name matches (got '" + lastSnapshot[0].deviceName + "')");
                check(lastSnapshot[0].servicePort == 2121, "discovered service port matches (got " +
                                                                std::to_string(lastSnapshot[0].servicePort) + ")");
                check(!lastSnapshot[0].address.empty(), "discovered device has a non-empty source address");
            }
        }

        // 广播间隔 150ms,等够多个周期(但小于 staleTimeout=800ms),同一个设备
        // 不该被反复判定为"变化"——只有名字变化才该重新触发回调。
        const int countAfterFirstDiscovery = [&] {
            std::lock_guard<std::mutex> lock(updatesMutex);
            return updateCount;
        }();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const int countAfterSteadyPeriod = [&] {
            std::lock_guard<std::mutex> lock(updatesMutex);
            return updateCount;
        }();
        check(countAfterSteadyPeriod == countAfterFirstDiscovery,
              "repeated broadcasts from the same unchanged device do not re-trigger the update callback (before=" +
                  std::to_string(countAfterFirstDiscovery) + ", after=" + std::to_string(countAfterSteadyPeriod) +
                  ")");

        beacon.stop();
    }
    // beacon 已经停止广播,staleTimeout=800ms 之后 listener 应该把它从列表里过期移除,
    // 并且这个"移除"本身要触发一次新的 update 回调。
    check(waitUntil(
              [&] {
                  std::lock_guard<std::mutex> lock(updatesMutex);
                  return lastSnapshot.empty();
              },
              3000),
          "stale device is automatically removed after the beacon stops broadcasting");

    listener.stop();
    check(!listener.isRunning(), "DiscoveryListener::stop() actually stops the listener");

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
