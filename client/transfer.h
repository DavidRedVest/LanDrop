#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QList>
#include <QMap>

#include <map>
#include <memory>
#include <mutex>

namespace core {
class FtpTransferManager;
enum class FtpTaskState;
} // namespace core

struct TransferTask {
    enum class Direction { Upload, Download };
    enum class State { Queued, Requesting, Transferring, Completed, Failed, Paused, Cancelled };

    QString id;
    Direction direction = Direction::Upload;
    QString localPath;   // 本地绝对路径
    QString remotePath;  // 远程相对路径
    qint64 totalSize = 0;
    qint64 bytesTransferred = 0;
    qint64 bytesPerSecond = 0;
    State state = State::Queued;
    QString statusMessage;

    qint64 lastSpeedSampleBytes = 0;
    qint64 lastSpeedSampleMsec = 0;
};

// 传输队列:标准 FTP 一条控制连接同一时刻只能做一件事(RETR/STOR 期间占用到收到
// 最终 226 为止),不再像旧自定义协议那样能在一条控制通道上给多个数据连接发令牌。
// 要支持"同时传几个文件",内部改为持有一个 core::FtpTransferManager——它自己维护
// 一个固定大小的、登录后常驻复用的 FTP 连接池(默认 3 条),不再依赖 Connection
// 的控制通道去申请传输令牌,所以构造函数不再需要 Connection* 参数(这是这次重写
// 唯一一处需要调用方跟着改的地方:MainWindow 需要在登录成功后调用
// setConnectionInfo() 告诉这里连去哪、用什么账号)。
//
// 暂停 = 让当前处理这个任务的 worker 中断当前传输,保留本地部分文件;
// 继续 = 重新入队,从已记录的字节数续传;失败自动重试(带退避、有上限)——这些
// 语义都不变,只是底层实现从"关闭自定义数据连接"换成了调用
// core::FtpTransferManager 对应的方法。
class TransferQueue : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColDirection = 0, ColName, ColProgress, ColSpeed, ColEta, ColStatus, ColumnCount };
    enum Role { BytesTransferredRole = Qt::UserRole + 1, TotalSizeRole, StateRole };

    explicit TransferQueue(QObject* parent = nullptr);
    ~TransferQueue() override;

    // 登录成功后调用一次(见 MainWindow::onLoginResult())。如果之前已经设置过
    // (比如断线重连/换了个站点),旧的连接池会先被停掉,不会遗留后台线程。
    void setConnectionInfo(const QString& host, quint16 port, const QString& username, const QString& password);

    void enqueueUpload(const QString& localPath, const QString& remoteDir);
    void enqueueDownload(const QString& remotePath, const QString& localDir, qint64 remoteSize);

    void pauseRow(int row);
    void resumeRow(int row);
    void cancelRow(int row);
    void removeRow(int row);
    void clearFinished();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    void updateRow(int row);
    int rowForId(const QString& id) const;
    // 下面两个跑在 UI 线程上,由 core::FtpTransferManager 的回调通过
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection) 调用过来。
    void applyProgress(const QString& id, qint64 bytesTransferred);
    void applyStateChange(const QString& id, core::FtpTaskState state, const QString& message);

    QList<TransferTask> m_tasks;
    int m_maxConcurrent = 3;

    // progress 回调来自 FtpTransferManager 任意 worker 线程,可能多个任务并发
    // 上报,节流(~200ms,和旧版 onWorkerProgress 的间隔一致)状态必须加锁保护,
    // 而且节流判断在跨线程 invokeMethod 之前做,不是转发过去之后才做——否则
    // 高频调用会直接把 Qt 事件队列灌爆,这正是之前大文件传输那次真实踩过的坑。
    std::mutex m_progressThrottleMutex;
    std::map<QString, qint64> m_lastProgressReportMsec;

    // 放在最后声明:C++ 成员按声明的反序析构,必须保证这个先于上面的
    // m_tasks/节流状态被销毁——它的析构会 stop() 并 join 所有 worker 线程,
    // 这样才能保证 join 完成后,不会再有任何回调线程尝试访问已经析构了一半的
    // TransferQueue。
    std::unique_ptr<core::FtpTransferManager> m_ftpManager;
};
