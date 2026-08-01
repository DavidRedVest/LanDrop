#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QList>
#include <QMap>

class Connection;
class ClientTransferWorker;

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
    int retryCount = 0;

    qint64 lastSpeedSampleBytes = 0;
    qint64 lastSpeedSampleMsec = 0;
};

// 数据通道 worker 的结束原因:成功 / 用户暂停 / 用户取消(都不重试) / 出错(会自动重试)
enum class TransferOutcome { Success, Paused, Cancelled, Error };

// 传输队列:入队、限并发发起(经 Connection 控制通道请求 token)、
// 实际字节收发由每个任务各自的 ClientTransferWorker(独立数据通道连接)完成。
// 失败自动重试(带退避、有上限);暂停 = 断开当前数据连接但保留本地部分文件,
// 之后"继续"会复用服务端的续传偏移机制从断点继续,而不是重新设计一套暂停协议。
class TransferQueue : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColDirection = 0, ColName, ColProgress, ColSpeed, ColEta, ColStatus, ColumnCount };
    enum Role { BytesTransferredRole = Qt::UserRole + 1, TotalSizeRole, StateRole };

    explicit TransferQueue(Connection* connection, QObject* parent = nullptr);

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

private slots:
    void onTransferReady(bool success, const QString& message, const QString& token, qint64 resumeOffset);

private:
    void startNextIfPossible();
    void startTask(int row);
    void updateRow(int row);
    int rowForId(const QString& id) const;
    void onWorkerProgress(int row, qint64 bytesTransferred);
    void onWorkerFinished(int row, TransferOutcome outcome, const QString& message);
    void scheduleRetry(int row);

    Connection* m_connection;
    QList<TransferTask> m_tasks;
    QString m_pendingRequestTaskId;
    int m_maxConcurrent = 3;
    int m_activeCount = 0;

    QMap<QString, ClientTransferWorker*> m_workers;
};
