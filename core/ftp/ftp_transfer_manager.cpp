#include "ftp_transfer_manager.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace core {

namespace {
constexpr int kMaxAutoRetries = 3;
constexpr int kRetryBackoffStepMs = 2000; // 线性退避,和旧 Qt 版本的 min(retryCount,5)*2000ms 一致
constexpr int kMaxRetryBackoffSteps = 5;
constexpr int kConnectTimeoutMs = 5000;
} // namespace

FtpTransferManager::FtpTransferManager(std::string host, uint16_t port, std::string username, std::string password,
                                        int maxConcurrent)
    : m_host(std::move(host)), m_port(port), m_username(std::move(username)), m_password(std::move(password)),
      m_maxConcurrent(maxConcurrent) {}

FtpTransferManager::~FtpTransferManager() {
    stop();
}

void FtpTransferManager::start() {
    if (m_started.exchange(true)) return;
    m_stopRequested = false;
    for (int i = 0; i < m_maxConcurrent; ++i) {
        auto slot = std::make_unique<WorkerSlot>();
        slot->client.reset(new FtpClient());
        WorkerSlot* rawSlot = slot.get();
        m_workers.push_back(std::move(slot));
        rawSlot->thread = std::thread(&FtpTransferManager::workerLoop, this, std::ref(*rawSlot));
    }
    m_retryThread = std::thread(&FtpTransferManager::retryLoop, this);
}

void FtpTransferManager::stop() {
    if (!m_started.exchange(false)) return;
    m_stopRequested = true;
    m_tasksCv.notify_all();
    m_retryCv.notify_all();

    for (auto& slot : m_workers) {
        // 唤醒可能卡在 select()/recv() 里的 worker(如果它正在处理一个传输)。
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->client->cancel();
    }
    for (auto& slot : m_workers) {
        if (slot->thread.joinable()) slot->thread.join();
    }
    m_workers.clear();

    if (m_retryThread.joinable()) m_retryThread.join();
}

std::string FtpTransferManager::nextTaskId() {
    return "task-" + std::to_string(m_taskCounter.fetch_add(1) + 1);
}

std::string FtpTransferManager::enqueueUpload(std::string localPath, std::string remotePath, uint64_t localFileSize) {
    TaskRecord record;
    record.task.id = nextTaskId();
    record.task.direction = FtpTransferTask::Direction::Upload;
    record.task.localPath = std::move(localPath);
    record.task.remotePath = std::move(remotePath);
    record.task.totalSize = localFileSize;

    const std::string id = record.task.id;
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        m_tasks.push_back(std::move(record));
    }
    m_tasksCv.notify_one();
    return id;
}

std::string FtpTransferManager::enqueueDownload(std::string remotePath, std::string localPath, uint64_t remoteSize) {
    TaskRecord record;
    record.task.id = nextTaskId();
    record.task.direction = FtpTransferTask::Direction::Download;
    record.task.remotePath = std::move(remotePath);
    record.task.localPath = std::move(localPath);
    record.task.totalSize = remoteSize;

    const std::string id = record.task.id;
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        m_tasks.push_back(std::move(record));
    }
    m_tasksCv.notify_one();
    return id;
}

template <typename Fn>
void FtpTransferManager::updateTask(const std::string& id, Fn&& mutator) {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    for (auto& record : m_tasks) {
        if (record.task.id == id) {
            mutator(record);
            return;
        }
    }
}

void FtpTransferManager::requestCancel(const std::string& id, CancelReason reason) {
    bool wasQueued = false;
    updateTask(id, [&](TaskRecord& record) {
        if (record.task.state == FtpTaskState::Queued) {
            wasQueued = true;
            record.task.state =
                reason == CancelReason::Pause ? FtpTaskState::Paused : FtpTaskState::Cancelled;
        } else if (record.task.state == FtpTaskState::Connecting || record.task.state == FtpTaskState::Transferring) {
            record.pendingCancel = reason;
        }
    });
    if (wasQueued) {
        reportState(id, reason == CancelReason::Pause ? FtpTaskState::Paused : FtpTaskState::Cancelled, {});
        return;
    }

    // 任务正在被某个 worker 处理:找到那个 worker,调用它的 FtpClient::cancel()
    // 中断当前 upload/downloadFile 调用(它会返回 FtpResult::Cancelled,runTask()
    // 里再根据上面记的 pendingCancel 决定最终状态是 Paused 还是 Cancelled)。
    for (auto& slot : m_workers) {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->currentTaskId == id) {
            slot->client->cancel();
            break;
        }
    }
}

void FtpTransferManager::pauseTask(const std::string& id) {
    requestCancel(id, CancelReason::Pause);
}

void FtpTransferManager::cancelTask(const std::string& id) {
    requestCancel(id, CancelReason::Cancel);
}

void FtpTransferManager::resumeTask(const std::string& id) {
    bool shouldWake = false;
    updateTask(id, [&](TaskRecord& record) {
        if (record.task.state == FtpTaskState::Paused || record.task.state == FtpTaskState::Failed ||
            record.task.state == FtpTaskState::Cancelled) {
            record.task.state = FtpTaskState::Queued;
            record.task.retryCount = 0;
            record.task.statusMessage.clear();
            record.pendingCancel = CancelReason::None;
            shouldWake = true;
        }
    });
    if (shouldWake) {
        reportState(id, FtpTaskState::Queued, {});
        m_tasksCv.notify_one();
    }
}

void FtpTransferManager::removeTask(const std::string& id) {
    // 如果任务正在被某个 worker 处理,先按"硬取消"中断它——worker 处理完这次
    // FtpResult::Cancelled 之后会尝试 updateTask() 查找这个 id,届时已经找不到,
    // 是安全的空操作(和旧 Qt 版本"worker 可能引用一个已经不在队列里的任务"的
    // 既有处理方式一致)。
    for (auto& slot : m_workers) {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->currentTaskId == id) {
            slot->client->cancel();
            break;
        }
    }
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(),
                                  [&](const TaskRecord& r) { return r.task.id == id; }),
                  m_tasks.end());
}

void FtpTransferManager::clearFinished() {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(),
                                  [](const TaskRecord& r) {
                                      return r.task.state == FtpTaskState::Completed ||
                                             r.task.state == FtpTaskState::Cancelled;
                                  }),
                  m_tasks.end());
}

std::vector<FtpTransferTask> FtpTransferManager::snapshot() const {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    std::vector<FtpTransferTask> result;
    result.reserve(m_tasks.size());
    for (const auto& record : m_tasks) result.push_back(record.task);
    return result;
}

void FtpTransferManager::reportProgress(const std::string& id, uint64_t bytesTransferred) {
    updateTask(id, [&](TaskRecord& record) { record.task.bytesTransferred = bytesTransferred; });
    if (m_onProgress) m_onProgress(id, bytesTransferred);
}

void FtpTransferManager::reportState(const std::string& id, FtpTaskState state, const std::string& message) {
    if (m_onState) m_onState(id, state, message);
}

// ---- worker 线程 ----

bool FtpTransferManager::waitForQueuedTask(std::string& outId) {
    std::unique_lock<std::mutex> lock(m_tasksMutex);
    m_tasksCv.wait(lock, [this] {
        if (m_stopRequested.load()) return true;
        return std::any_of(m_tasks.begin(), m_tasks.end(),
                            [](const TaskRecord& r) { return r.task.state == FtpTaskState::Queued; });
    });
    if (m_stopRequested.load()) return false;

    for (auto& record : m_tasks) {
        if (record.task.state == FtpTaskState::Queued) {
            record.task.state = FtpTaskState::Connecting;
            record.pendingCancel = CancelReason::None;
            outId = record.task.id;
            return true;
        }
    }
    return false; // 被别的 worker 抢走了,回到外层循环重新等待
}

void FtpTransferManager::workerLoop(WorkerSlot& slot) {
    while (!m_stopRequested.load()) {
        std::string taskId;
        if (!waitForQueuedTask(taskId)) continue;

        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.currentTaskId = taskId;
        }

        // 整个任务处理过程包一层 try/catch:这是这个 worker 线程的顶层循环体,
        // 任何未捕获异常逃出去都会 std::terminate() 整个客户端进程(同一类问题
        // 见 core/ftp/ftp_session.cpp 里 FtpSession::run() 的注释)。抓住后把这个
        // 任务标记失败、走正常的自动重试路径,这个 worker 本身留着继续处理队列
        // 里的下一个任务,不影响其它并发任务。
        bool caughtException = false;
        try {
            if (!slot.client->isConnected()) {
                const FtpResult connectResult = slot.client->connect(m_host, m_port, kConnectTimeoutMs);
                const FtpResult loginResult =
                    connectResult == FtpResult::Ok ? slot.client->login(m_username, m_password) : connectResult;
                if (loginResult != FtpResult::Ok) {
                    int retryCount = 0;
                    updateTask(taskId, [&](TaskRecord& record) {
                        record.task.state = FtpTaskState::Failed;
                        record.task.statusMessage = "connect/login failed";
                        ++record.task.retryCount;
                        retryCount = record.task.retryCount;
                    });
                    reportState(taskId, FtpTaskState::Failed, "connect/login failed");
                    if (retryCount <= kMaxAutoRetries) scheduleRetry(taskId, retryCount);

                    std::lock_guard<std::mutex> lock(slot.mutex);
                    slot.currentTaskId.clear();
                    continue;
                }
            }

            runTask(*slot.client, taskId);
        } catch (const std::exception&) {
            caughtException = true;
        } catch (...) {
            caughtException = true;
        }

        if (caughtException) {
            int retryCount = 0;
            updateTask(taskId, [&](TaskRecord& record) {
                record.task.state = FtpTaskState::Failed;
                record.task.statusMessage = "unexpected internal error";
                ++record.task.retryCount;
                retryCount = record.task.retryCount;
            });
            reportState(taskId, FtpTaskState::Failed, "unexpected internal error");
            if (retryCount <= kMaxAutoRetries) scheduleRetry(taskId, retryCount);
        }

        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.currentTaskId.clear();
    }

    if (slot.client->isConnected()) slot.client->disconnect();
}

void FtpTransferManager::runTask(FtpClient& client, const std::string& id) {
    FtpTransferTask task;
    bool found = false;
    updateTask(id, [&](TaskRecord& record) {
        record.task.state = FtpTaskState::Transferring;
        task = record.task;
        found = true;
    });
    if (!found) return; // 在真正开始前就被 removeTask() 移除了
    reportState(id, FtpTaskState::Transferring, {});

    client.setProgressCallback([this, &id](uint64_t transferred, uint64_t /*total*/) {
        reportProgress(id, transferred);
    });

    const FtpResult result = task.direction == FtpTransferTask::Direction::Upload
                                  ? client.uploadFile(task.localPath, task.remotePath, task.bytesTransferred)
                                  : client.downloadFile(task.remotePath, task.localPath, task.bytesTransferred);
    client.setProgressCallback(nullptr);

    if (result == FtpResult::Ok) {
        updateTask(id, [](TaskRecord& record) {
            record.task.state = FtpTaskState::Completed;
            record.task.bytesTransferred = record.task.totalSize;
            record.task.statusMessage.clear();
            record.task.retryCount = 0;
        });
        reportState(id, FtpTaskState::Completed, {});
        return;
    }

    if (result == FtpResult::Cancelled) {
        CancelReason reason = CancelReason::Cancel;
        updateTask(id, [&](TaskRecord& record) {
            reason = record.pendingCancel == CancelReason::None ? CancelReason::Cancel : record.pendingCancel;
            record.task.state = reason == CancelReason::Pause ? FtpTaskState::Paused : FtpTaskState::Cancelled;
            record.pendingCancel = CancelReason::None;
        });
        reportState(id, reason == CancelReason::Pause ? FtpTaskState::Paused : FtpTaskState::Cancelled, {});
        return;
    }

    // 其它失败(连接失败/IO 错误/超时等):记失败原因,未超过重试上限就安排自动重试。
    int retryCount = 0;
    std::string message;
    updateTask(id, [&](TaskRecord& record) {
        ++record.task.retryCount;
        retryCount = record.task.retryCount;
        message = retryCount <= kMaxAutoRetries ? ("传输失败,将自动重试(第 " + std::to_string(retryCount) + " 次)")
                                                 : "传输失败";
        record.task.statusMessage = message;
        record.task.state = FtpTaskState::Failed;
    });
    reportState(id, FtpTaskState::Failed, message);
    if (retryCount <= kMaxAutoRetries) scheduleRetry(id, retryCount);
}

// ---- 失败重试调度:和 Heartbeat 一样用 cv 做可中断等待,不是 sleep_for 轮询 ----

void FtpTransferManager::scheduleRetry(const std::string& id, int retryCount) {
    const int steps = std::min(retryCount, kMaxRetryBackoffSteps);
    const auto dueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(steps * kRetryBackoffStepMs);
    {
        std::lock_guard<std::mutex> lock(m_retryMutex);
        m_pendingRetries.push_back({id, dueAt});
    }
    m_retryCv.notify_all();
}

void FtpTransferManager::retryLoop() {
    std::unique_lock<std::mutex> lock(m_retryMutex);
    while (!m_stopRequested.load()) {
        if (m_pendingRetries.empty()) {
            m_retryCv.wait(lock, [this] { return m_stopRequested.load() || !m_pendingRetries.empty(); });
            continue;
        }

        const auto nextDue =
            std::min_element(m_pendingRetries.begin(), m_pendingRetries.end(),
                              [](const RetryEntry& a, const RetryEntry& b) { return a.dueAt < b.dueAt; })
                ->dueAt;

        if (m_retryCv.wait_until(lock, nextDue, [this] { return m_stopRequested.load(); })) {
            break; // 被 stop() 唤醒
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> dueIds;
        m_pendingRetries.erase(std::remove_if(m_pendingRetries.begin(), m_pendingRetries.end(),
                                               [&](const RetryEntry& e) {
                                                   if (e.dueAt > now) return false;
                                                   dueIds.push_back(e.taskId);
                                                   return true;
                                               }),
                                m_pendingRetries.end());

        lock.unlock();
        for (const auto& id : dueIds) {
            bool requeued = false;
            updateTask(id, [&](TaskRecord& record) {
                if (record.task.state != FtpTaskState::Failed) return; // 用户可能已手动取消/移除
                record.task.state = FtpTaskState::Queued;
                requeued = true;
            });
            if (requeued) reportState(id, FtpTaskState::Queued, {});
        }
        if (!dueIds.empty()) m_tasksCv.notify_all();
        lock.lock();
    }
}

} // namespace core
