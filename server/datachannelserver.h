#pragma once

#include <QTcpServer>
#include <QMap>
#include <QString>

class FileManager;

// 监听 DATA_PORT 的第二个 QTcpServer,负责实际文件字节的收发。
//
// 流程:控制通道上的 Session 先调用 registerPendingTransfer() 换一个一次性
// token,连同 resumeOffset 一起通过 TRANSFER_READY 告诉客户端;客户端随后单独
// 连一条 socket 到这里,第一个包必须是 DATA_CHANNEL_AUTH(payload=token),
// 认领成功后开始收发 TRANSFER_DATA,直到 TRANSFER_DONE。
class DataChannelServer : public QTcpServer {
    Q_OBJECT

public:
    explicit DataChannelServer(FileManager* fileManager, QObject* parent = nullptr);

    void setMaxConcurrent(int maxConcurrent) { m_maxConcurrent = maxConcurrent; }
    int activeTransferCount() const;

    // clientIsUploading: true 表示客户端要把数据发给服务端(服务端写文件),
    // false 表示客户端要从服务端下载(服务端读文件发送)。
    // clientReportedSize: 上传时是源文件总大小;下载时是客户端本地已有字节数。
    // 返回一次性 token,outResumeOffset 是服务端计算/钳制后的实际续传偏移。
    QString registerPendingTransfer(const QString& relativePath, bool clientIsUploading,
                                     qint64 clientReportedSize, qint64& outResumeOffset);
    void cancelTransfer(const QString& token);

signals:
    void logMessage(const QString& message);
    void transferStarted(const QString& token, const QString& fileName, bool isUpload);
    void transferProgress(const QString& token, qint64 bytesTransferred, qint64 totalBytes);
    void transferCompleted(const QString& token, const QString& fileName);
    void transferFailed(const QString& token, const QString& errorMessage);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    struct PendingTransfer {
        QString relativePath;
        bool clientIsUploading = false;
        qint64 resumeOffset = 0;
        qint64 expectedTotalSize = 0;
    };

    FileManager* m_fileManager;
    QMap<QString, PendingTransfer> m_pending;
    int m_maxConcurrent = 4;
    int m_activeCount = 0;

    friend class DataTransferWorker;
    bool hasCapacity() const { return m_activeCount < m_maxConcurrent; }
    bool takePendingTransfer(const QString& token, PendingTransfer& outTransfer);
    void onWorkerStarted();
    void onWorkerFinished();
};
