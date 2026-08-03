#pragma once

// 客户端并发传输池:标准 FTP 的控制连接同一时刻只能做一件事(RETR/STOR 期间
// 控制通道被占用到收到最终 226 为止),不像旧自定义协议那样一条控制通道能同时
// 给多个数据连接发令牌。要支持"同时传几个文件",只能维护多条独立的 FTP 会话——
// 这是 FileZilla 等主流多连接 FTP 客户端的标准做法,不是这里额外发明的复杂度。
//
// 内部是一个固定大小的 worker 池,每个 worker 是一条独立、登录后常驻复用的
// core::FtpClient + 一条专属 std::thread,循环从任务队列取任务执行,不是每个
// 任务重新连接一次。不依赖 Qt——GUI 侧(client/transfer.*)通过回调对接。

#include "ftp_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

enum class FtpTaskState { Queued, Connecting, Transferring, Completed, Failed, Paused, Cancelled };

struct FtpTransferTask {
    enum class Direction { Upload, Download };

    std::string id;
    Direction direction = Direction::Upload;
    std::string localPath;  // 本地绝对路径
    std::string remotePath; // 远程虚拟路径
    uint64_t totalSize = 0;
    uint64_t bytesTransferred = 0;
    FtpTaskState state = FtpTaskState::Queued;
    std::string statusMessage;
    int retryCount = 0;
};

using TaskProgressCallback = std::function<void(const std::string& taskId, uint64_t bytesTransferred)>;
using TaskStateCallback =
    std::function<void(const std::string& taskId, FtpTaskState state, const std::string& message)>;

class FtpTransferManager {
public:
    FtpTransferManager(std::string host, uint16_t port, std::string username, std::string password,
                        int maxConcurrent = 3);
    ~FtpTransferManager();

    FtpTransferManager(const FtpTransferManager&) = delete;
    FtpTransferManager& operator=(const FtpTransferManager&) = delete;

    void setProgressCallback(TaskProgressCallback cb) { m_onProgress = std::move(cb); }
    void setStateCallback(TaskStateCallback cb) { m_onState = std::move(cb); }

    // 启动 worker 线程池。每个 worker 在真正取到第一个任务时才建立连接+登录
    // (懒连接),之后这条连接常驻复用,不是每个任务重新连一次。
    void start();
    // 唤醒所有可能卡在"等任务"上的 worker 线程和重试调度线程,请求它们退出并 join。
    // 析构函数会自动调用,可安全重复调用。
    void stop();

    // localFileSize/remoteSize 由调用方(Qt 层已经查询过本地/远程文件信息)传入,
    // 这里不做任何文件系统或网络查询,只负责排队和调度。
    std::string enqueueUpload(std::string localPath, std::string remotePath, uint64_t localFileSize);
    std::string enqueueDownload(std::string remotePath, std::string localPath, uint64_t remoteSize);

    // 暂停/继续/取消/删除语义和旧 TransferQueue 完全一致:
    // 暂停 = 让当前正在处理这个任务的 worker 调用 FtpClient::cancel() 中断当前
    // 传输(数据连接关闭,本地部分文件保留),任务记为 Paused;
    // 继续 = 重新入队,resumeOffset 用任务已记录的 bytesTransferred(REST 续传);
    // 取消 = 语义同暂停,但任务记为 Cancelled,不会被 resumeTask() 复活。
    //
    // 已知限制(下载方向不受影响,只影响"暂停上传后再续传"这一种场景):
    // 上传续传的 offset 用的是客户端自己统计的 bytesTransferred(每次 sendSome()
    // 成功后累加),这只保证"本地 OS 的发送缓冲区已经接受了这些字节",不是"服务端
    // 已经把这些字节落盘"的确认——RFC 959 本身没有让服务端在传输中途报告"已收到
    // 多少字节"的机制(RFC 3659 的 SIZE 扩展命令可以做到,但当前 core::FtpServer
    // 没有实现,不在本次改动范围内)。极端情况下(暂停发生的那一瞬间,已统计但还
    // 未被服务端实际写盘的字节丢失)续传后的文件可能有极小概率损坏。局域网内正常
    // 使用这个概率可以忽略不计,但这是协议标准化后的真实取舍,不是遗漏——和
    // CLAUDE.md 里记录的"标准 FTP 没有内建完整性校验""服务端未实现 ABOR"是同一类
    // 已知、可接受的简化。
    void pauseTask(const std::string& id);
    void resumeTask(const std::string& id);
    void cancelTask(const std::string& id);
    void removeTask(const std::string& id);
    void clearFinished();

    // 当前所有任务的快照,按插入顺序——供 Qt 层刷新表格模型用,不持有内部锁。
    std::vector<FtpTransferTask> snapshot() const;

private:
    struct WorkerSlot {
        std::thread thread;
        std::unique_ptr<FtpClient> client;
        mutable std::mutex mutex; // 保护 currentTaskId,供主线程的 pause/cancel 查询
        std::string currentTaskId;
    };
    struct RetryEntry {
        std::string taskId;
        std::chrono::steady_clock::time_point dueAt;
    };
    enum class CancelReason { None, Pause, Cancel };

    // 内部任务记录:在公开的 FtpTransferTask 基础上多带一个"暂停 vs 取消"的挂起
    // 意图标记——FtpClient::cancel() 本身不区分这两者(都只是让当前 upload/
    // downloadFile 调用返回 FtpResult::Cancelled),需要 manager 自己记住调用方
    // 到底是想暂停(可续传)还是彻底取消(不会被 resumeTask 复活)。
    struct TaskRecord {
        FtpTransferTask task;
        CancelReason pendingCancel = CancelReason::None;
    };

    void workerLoop(WorkerSlot& slot);
    bool waitForQueuedTask(std::string& outId);
    void runTask(FtpClient& client, const std::string& id);
    void requestCancel(const std::string& id, CancelReason reason);

    template <typename Fn>
    void updateTask(const std::string& id, Fn&& mutator);

    void reportProgress(const std::string& id, uint64_t bytesTransferred);
    void reportState(const std::string& id, FtpTaskState state, const std::string& message);
    void scheduleRetry(const std::string& id, int retryCount);
    void retryLoop();
    std::string nextTaskId();

    std::string m_host;
    uint16_t m_port;
    std::string m_username;
    std::string m_password;
    int m_maxConcurrent;

    mutable std::mutex m_tasksMutex;
    std::condition_variable m_tasksCv; // 有新的 Queued 任务或停止请求时唤醒等待中的 worker
    std::vector<TaskRecord> m_tasks;   // 按插入顺序,和旧 QList<TransferTask> 的行序语义一致
    // 跨实例共享(static):GUI 层(TransferQueue)每次重连都会 stop() 旧的、new 一个
    // 新的 FtpTransferManager,但旧连接会话里已完成的任务行还留在 TransferQueue::
    // m_tasks 里不会被清空。如果这里是普通成员变量,新 manager 会从 0 重新计数,
    // 新任务的 id(如 "task-1")就会撞上上一次会话里同名的旧行——TransferQueue::
    // rowForId() 按插入顺序找到的是先出现的那个旧行,新任务的 progress/state 回调
    // 全部被错误地写到那一行上,真正的新行永远收不到更新,表现为 UI 上永远"排队中"
    // 且没有进度条(实测复现:断线重连后正是这个现象)。计数器改成进程级共享,保证
    // id 在整个进程生命周期内不重复,不管重连/重建多少次。
    static std::atomic<uint64_t> s_taskCounter;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_started{false};

    std::vector<std::unique_ptr<WorkerSlot>> m_workers;

    std::thread m_retryThread;
    std::mutex m_retryMutex;
    std::condition_variable m_retryCv;
    std::vector<RetryEntry> m_pendingRetries;

    TaskProgressCallback m_onProgress;
    TaskStateCallback m_onState;
};

} // namespace core
