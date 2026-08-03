#pragma once

#include <QObject>
#include <QString>
#include <QList>

#include "../common/ftptypes.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace core {
class FtpClient;
}

// 客户端控制通道:登录、浏览、文件管理。内部包一条独立 std::thread,顺序执行
// 一个任务队列,每个任务对这条线程私有的 core::FtpClient 做一次同步阻塞调用,
// 结果通过 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 安全地跨线程
// 转发成 Qt 信号——公开接口(方法名/信号)和旧的自定义协议实现完全一致,调用方
// (MainWindow/FileBrowserPanel)不需要改。
//
// 传输本身(上传/下载)不再经过这条控制通道:标准 FTP 一条控制连接同一时刻只能
// 做一件事,并发传输需要独立的 FTP 会话,由 TransferQueue 自己的
// core::FtpTransferManager 连接池负责,不再需要这里的"申请传输令牌"握手,所以
// 旧版本的 requestUpload/requestDownload/cancelTransfer/transferReady/dataPort()
// 在这次重写里被移除——已确认除了旧版 TransferQueue 之外没有别的调用方。
// changeDir()/directoryChanged 信号同样被移除:确认过旧代码里也从未被真正调用过
// (GUI 从来没有连接过 directoryChanged),不是这次重写才产生的死代码。
class Connection : public QObject {
    Q_OBJECT

public:
    explicit Connection(QObject* parent = nullptr);
    ~Connection() override;

    void connectToHost(const QString& host, quint16 port = FTP::DEFAULT_PORT);
    void disconnectFromHost();
    bool isConnected() const { return m_connected.load(); }

    QString host() const { return m_host; }
    quint16 controlPort() const { return m_port; }

    void login(const QString& username, const QString& password);
    void listDirectory(const QString& path);
    void mkdir(const QString& path);
    void rmdir(const QString& path);
    void deleteFile(const QString& path);
    void rename(const QString& oldPath, const QString& newPath);

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void loginResult(bool success, const QString& message);
    void directoryListed(bool success, const QString& path, const QString& message, const QList<FTP::FileInfo>& list);
    void operationResult(bool success, const QString& message);

private:
    using Job = std::function<void(core::FtpClient&)>;

    void threadMain();
    void pushJob(Job job);
    // 只在 worker 线程上被读写(setErrorCallback 的回调和每个 job 完成后都在
    // 这条线程上跑),不需要额外加锁。取出 core::FtpClient 报的真实错误文本
    // (比如服务器实际回复的"530 Login incorrect"),取不到时才退回一个通用提示——
    // 之前这里所有失败场景都是硬编码的中文提示,FtpClient 自己通过
    // ErrorCallback 报出来的真实原因根本没接上、直接被扔掉了,导致"登录失败"这类
    // 提示看不出到底是密码错、服务器要求 TLS、账号被限制访问,还是别的协议层原因。
    QString takeLastErrorOr(const QString& fallback);

    QString m_host;
    quint16 m_port = FTP::DEFAULT_PORT;
    std::atomic<bool> m_connected{false};
    std::string m_lastError;

    std::thread m_thread;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Job> m_jobs;
    std::atomic<bool> m_stopRequested{false};
};
