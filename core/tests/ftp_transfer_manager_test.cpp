// core::FtpTransferManager 的真实集成测试:一个真实 core::FtpServer(根目录是磁盘上
// 的真实临时目录)被真实 FtpTransferManager 通过真实回环 TCP 驱动,覆盖并发上传/
// 下载、暂停后续传、失败自动重试、取消——延续本项目一贯的验证方法论(实际跑一遍,
// 不是只看代码走查)。
#include "../ftp/ftp_server.h"
#include "../ftp/ftp_transfer_manager.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writePatternFile(const std::string& path, size_t size, int seed) {
    std::ofstream out(path, std::ios::binary);
    for (size_t i = 0; i < size; ++i) out.put(static_cast<char>((static_cast<int>(i) * 7 + seed) % 251));
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool findTask(core::FtpTransferManager& mgr, const std::string& id, core::FtpTransferTask& out) {
    for (auto& t : mgr.snapshot()) {
        if (t.id == id) {
            out = t;
            return true;
        }
    }
    return false;
}

bool allCompleted(core::FtpTransferManager& mgr, const std::vector<std::string>& ids) {
    const auto snap = mgr.snapshot();
    for (const auto& id : ids) {
        bool found = false;
        for (const auto& t : snap) {
            if (t.id == id) {
                found = true;
                if (t.state != core::FtpTaskState::Completed) return false;
            }
        }
        if (!found) return false;
    }
    return true;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path serverRoot = fs::temp_directory_path() / "landrop_transfer_manager_test_server";
    const fs::path localSrcDir = fs::temp_directory_path() / "landrop_transfer_manager_test_src";
    const fs::path localDstDir = fs::temp_directory_path() / "landrop_transfer_manager_test_dst";
    std::error_code ec;
    fs::remove_all(serverRoot, ec);
    fs::remove_all(localSrcDir, ec);
    fs::remove_all(localDstDir, ec);
    fs::create_directories(serverRoot, ec);
    fs::create_directories(localSrcDir, ec);
    fs::create_directories(localDstDir, ec);

    core::FtpServer server;
    server.setRootPath(serverRoot.string());
    server.setAuthenticator(
        [](const std::string& user, const std::string& pass) { return user == "pooluser" && pass == "poolpass"; });
    constexpr uint16_t kPort = 19580;
    check(server.start(kPort), "FtpServer::start binds and listens");

    // ---- 场景 1:并发上传 + 并发下载(worker 数量 < 任务数量,验证任务池调度) ----
    {
        core::FtpTransferManager mgr("127.0.0.1", kPort, "pooluser", "poolpass", /*maxConcurrent=*/2);
        mgr.start();

        const std::vector<std::string> names = {"a.bin", "b.bin", "c.bin"};
        std::vector<std::string> ids;
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string localPath = (localSrcDir / names[i]).string();
            writePatternFile(localPath, 300000 + i * 1000, static_cast<int>(i) + 1);
            const uint64_t size = fs::file_size(localPath);
            ids.push_back(mgr.enqueueUpload(localPath, "/" + names[i], size));
        }

        check(waitUntil([&] { return allCompleted(mgr, ids); }, 15000),
              "3 concurrent uploads (pool size 2) all reach Completed within timeout");

        bool allMatch = true;
        for (const auto& name : names) {
            const std::string src = readWholeFile((localSrcDir / name).string());
            const std::string dst = readWholeFile((serverRoot / name).string());
            if (src != dst || src.empty()) allMatch = false;
        }
        check(allMatch, "all 3 uploaded files match source content byte-for-byte on server disk");

        std::vector<std::string> downloadIds;
        for (const auto& name : names) {
            const uint64_t remoteSize = fs::file_size(serverRoot / name);
            downloadIds.push_back(
                mgr.enqueueDownload("/" + name, (localDstDir / name).string(), remoteSize));
        }
        check(waitUntil([&] { return allCompleted(mgr, downloadIds); }, 15000),
              "3 concurrent downloads (pool size 2) all reach Completed within timeout");

        bool allDownloadsMatch = true;
        for (const auto& name : names) {
            const std::string src = readWholeFile((localSrcDir / name).string());
            const std::string dst = readWholeFile((localDstDir / name).string());
            if (src != dst || src.empty()) allDownloadsMatch = false;
        }
        check(allDownloadsMatch, "all 3 downloaded files match source content byte-for-byte locally");

        mgr.stop();
    }

    // ---- 场景 2:暂停(在第一次 progress 回调里同步触发,确定性,不依赖时序竞争)+ 续传 ----
    {
        core::FtpTransferManager mgr("127.0.0.1", kPort, "pooluser", "poolpass", /*maxConcurrent=*/1);
        mgr.start();

        const std::string localPath = (localSrcDir / "pause_target.bin").string();
        writePatternFile(localPath, 4 * 1024 * 1024, 99); // 4MB,256KB 分块下有多个 chunk
        const uint64_t size = fs::file_size(localPath);

        std::atomic<bool> pauseTriggered{false};
        std::string targetId;
        mgr.setProgressCallback([&](const std::string& id, uint64_t /*bytesTransferred*/) {
            // 第一次收到这个任务的进度回调时立刻请求暂停——这个回调是从 worker
            // 线程内部、FtpClient 分块发送循环的同一次调用栈里同步触发的,所以
            // pauseTask() 设置的取消标志保证会在下一次分块发送前被检查到,不依赖
            // 猜测时间窗口的睡眠等待。
            if (id == targetId && !pauseTriggered.exchange(true)) {
                mgr.pauseTask(id);
            }
        });

        targetId = mgr.enqueueUpload(localPath, "/pause_target.bin", size);

        check(waitUntil(
                  [&] {
                      core::FtpTransferTask t;
                      return findTask(mgr, targetId, t) && t.state == core::FtpTaskState::Paused;
                  },
                  5000),
              "upload pauses (deterministically, via progress-callback-triggered pauseTask)");

        core::FtpTransferTask pausedTask;
        findTask(mgr, targetId, pausedTask);
        check(pausedTask.bytesTransferred > 0 && pausedTask.bytesTransferred < size,
              "paused task has partial bytesTransferred (got " + std::to_string(pausedTask.bytesTransferred) +
                  " of " + std::to_string(size) + ")");

        mgr.resumeTask(targetId);
        check(waitUntil(
                  [&] {
                      core::FtpTransferTask t;
                      return findTask(mgr, targetId, t) && t.state == core::FtpTaskState::Completed;
                  },
                  15000),
              "resumed upload reaches Completed");

        const std::string src = readWholeFile(localPath);
        const std::string dst = readWholeFile((serverRoot / "pause_target.bin").string());
        check(src == dst && !src.empty(), "resumed upload's final content matches original source byte-for-byte");

        mgr.stop();
    }

    // ---- 场景 3:失败自动重试(远程文件名含非法字符,服务端 isValidFtpName()
    // 必然拒绝、每次都回复 553——不能用"上传到不存在的目录"来触发,因为
    // FtpSession::handleStor() 会自动 create_directories() 建好父目录,那样根本
    // 不会失败。验证重试次数增长、最终耗尽重试次数后停在 Failed,过程不挂起) ----
    {
        core::FtpTransferManager mgr("127.0.0.1", kPort, "pooluser", "poolpass", /*maxConcurrent=*/1);
        mgr.start();

        const std::string localPath = (localSrcDir / "retry_target.bin").string();
        writePatternFile(localPath, 1024, 7);
        const uint64_t size = fs::file_size(localPath);

        const std::string id = mgr.enqueueUpload(localPath, "/bad:name.bin", size);

        // 3 次自动重试,线性退避 2s/4s/6s,总共大约 12s 多一点耗尽;给足 25s 超时。
        check(waitUntil(
                  [&] {
                      core::FtpTransferTask t;
                      return findTask(mgr, id, t) && t.state == core::FtpTaskState::Failed && t.retryCount >= 4;
                  },
                  25000),
              "upload to nonexistent remote directory exhausts auto-retries and settles on Failed");

        mgr.stop();
    }

    // ---- 场景 4:取消一个还在排队、尚未被任何 worker 处理的任务(确定性分支,
    // 不涉及任何 worker/网络交互) ----
    {
        core::FtpTransferManager mgr("127.0.0.1", kPort, "pooluser", "poolpass", /*maxConcurrent=*/1);
        mgr.start();

        const std::string busyPath = (localSrcDir / "busy.bin").string();
        writePatternFile(busyPath, 2 * 1024 * 1024, 3);
        const uint64_t busySize = fs::file_size(busyPath);
        const std::string busyId = mgr.enqueueUpload(busyPath, "/busy.bin", busySize); // 占住唯一的 worker

        const std::string queuedPath = (localSrcDir / "queued.bin").string();
        writePatternFile(queuedPath, 1024, 11);
        const uint64_t queuedSize = fs::file_size(queuedPath);
        const std::string queuedId = mgr.enqueueUpload(queuedPath, "/queued.bin", queuedSize);

        mgr.cancelTask(queuedId);
        core::FtpTransferTask cancelledTask;
        check(findTask(mgr, queuedId, cancelledTask) && cancelledTask.state == core::FtpTaskState::Cancelled,
              "cancelling a still-Queued task transitions it to Cancelled immediately, synchronously");

        check(waitUntil(
                  [&] {
                      core::FtpTransferTask t;
                      return findTask(mgr, busyId, t) && t.state == core::FtpTaskState::Completed;
                  },
                  10000),
              "the other (busy) task completes normally, unaffected by the cancelled queued task");

        mgr.stop();
    }

    server.stop();
    fs::remove_all(serverRoot, ec);
    fs::remove_all(localSrcDir, ec);
    fs::remove_all(localDstDir, ec);

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
