#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QQueue>
#include <QPair>
#include <QVector>

#include "../common/ftptypes.h"

class Connection;
class TransferQueue;

// 文件夹上传/下载协调器:标准 FTP 没有"递归传输整个目录"这种命令,这里负责把
// 一次"上传/下载文件夹"拆解成:
//   上传 = 本地递归遍历(QDirIterator)+ 远程逐级建目录(父目录在前,标准 FTP 的
//          MKD 大多数第三方服务端只建单层,不能假设会像 LanDrop 自己的服务端
//          那样隐式建好父目录),全部建完之后再把发现的文件正式入队,避免 STOR
//          抢在父目录建出来之前发生。
//   下载 = 远程目录树广度优先遍历(一次一个 LIST)+ 本地建目录(QDir::mkpath,
//          纯本地文件系统操作,不需要异步等待),发现的文件正式入队。
// 独立于 MainWindow、不依赖任何 QWidget,是为了能被 headless 的
// gui_core_integration_test 直接测试——和这个项目一直以来"核心逻辑单独可测,
// GUI 只是薄封装"的方法论一致(TransferQueue 包 core::FtpTransferManager、
// Connection 包 core::FtpClient 都是同一个道理)。
class FolderTransferCoordinator : public QObject {
    Q_OBJECT

public:
    FolderTransferCoordinator(Connection* connection, TransferQueue* transferQueue, QObject* parent = nullptr);

    // localFolderPaths 每项是本地绝对路径的一个目录;remoteBaseDir 是上传目标的
    // 远程父目录(通常取自远程面板当前路径)。
    void uploadFolders(const QStringList& localFolderPaths, const QString& remoteBaseDir);
    // remoteFolderPaths 每项是远程虚拟绝对路径的一个目录;localBaseDir 是下载目标
    // 的本地父目录(通常取自本地面板当前路径)。
    void downloadFolders(const QStringList& remoteFolderPaths, const QString& localBaseDir);

private slots:
    void onOperationResult(bool success, const QString& message);
    void onDirectoryListed(bool success, const QString& path, const QString& message,
                            const QList<FTP::FileInfo>& list);

private:
    void startNextUploadIfIdle();
    void startNextDownloadStepIfIdle();

    // 一批文件夹上传里的"一个根文件夹":这批目录全部建完之前,文件先存着不入队。
    struct PendingFolderUpload {
        QStringList remoteDirsToCreate; // 父目录在前
        QVector<QPair<QString, QString>> files; // (本地文件绝对路径, 目标远程目录)
    };

    Connection* m_connection;
    TransferQueue* m_transferQueue;

    // 一次只处理一个 PendingFolderUpload:Connection::operationResult 不带路径/
    // 任务信息,没法区分是哪次 mkdir 的结果,只能靠"一次只做一件事"保证对应关系
    // 正确。已知限制:文件夹上传进行中,如果用户又手动做了别的远程操作(删除/
    // 重命名/新建目录),也会触发 operationResult,可能把这里的计数搅乱、提前
    // 把文件入队——接受的简化,不是遗漏。
    QQueue<PendingFolderUpload> m_pendingUploads;
    int m_uploadMkdirRemaining = 0;
    bool m_uploadInFlight = false;
    QVector<QPair<QString, QString>> m_uploadPendingFiles;

    // 广度优先遍历用的队列:(远程目录, 对应的本地目录)。directoryListed 回调带回来
    // 的 path 和 m_downloadCurrentRemote 匹配,就是我们正在等待的这次响应——
    // Connection::listDirectory() 全程用绝对虚拟路径,不依赖服务端当前工作目录
    // 状态,所以哪怕和 FileBrowserPanel 自己独立的、用户手动浏览触发的
    // listDirectory() 调用交替执行,靠路径匹配也不会认错响应。
    QQueue<QPair<QString, QString>> m_downloadQueue;
    QString m_downloadCurrentRemote;
    QString m_downloadCurrentLocal;
    bool m_downloadInFlight = false;
};
