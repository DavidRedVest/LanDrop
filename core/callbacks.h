#pragma once

// 纯 std::function 回调接口——核心逻辑通过这些类型向外报告进度/状态/错误,
// 不知道、也不依赖 Qt 的存在。Qt 侧(client/、server/)写薄适配层把这些回调
// 转成 Qt 信号,用 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 安全地
// 从核心的工作线程跨线程传回 UI 线程。

#include <cstdint>
#include <functional>
#include <string>

namespace core {

enum class TransferState {
    Idle,
    Connecting,
    Transferring,
    Paused,
    Completed,
    Failed,
    Cancelled
};

using ProgressCallback = std::function<void(uint64_t bytesTransferred, uint64_t totalBytes)>;
using StateCallback = std::function<void(TransferState state)>;
using ErrorCallback = std::function<void(const std::string& message)>;
using LogCallback = std::function<void(const std::string& message)>;

} // namespace core
