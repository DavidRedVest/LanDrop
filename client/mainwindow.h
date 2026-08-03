#pragma once

#include <QMainWindow>

class QLabel;
class Connection;
class TransferQueue;
class FileBrowserPanel;
class TransferWidget;
class FolderTransferCoordinator;
struct SiteInfo;

// 客户端主窗口:左右两栏本地/远程文件浏览器 + 底部传输队列 + 站点管理入口。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenSiteManager();
    void onDisconnectClicked();

    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& message);
    void onLoginResult(bool success, const QString& message);

    void onLocalUploadRequested(const QStringList& localPaths);
    void onRemoteDownloadRequested(const QList<QPair<QString, qint64>>& remoteFiles);
    void onStatusMessage(const QString& message);

    // 文件夹上传/下载本身的递归遍历/建目录逻辑在 FolderTransferCoordinator 里
    // (独立于 QWidget,方便单独测试);这两个槽只负责校验连接状态、转发。
    void onFolderUploadRequested(const QStringList& localFolderPaths);
    void onFolderDownloadRequested(const QStringList& remoteFolderPaths);

private:
    void connectToSite(const SiteInfo& site);
    void setConnectedUiState(bool connected);

    Connection* m_connection;
    TransferQueue* m_transferQueue;
    FolderTransferCoordinator* m_folderTransfer;
    FileBrowserPanel* m_localPanel;
    FileBrowserPanel* m_remotePanel;
    TransferWidget* m_transferWidget;
    QLabel* m_statusLabel;

    QString m_pendingUsername;
    QString m_pendingPassword;

    class QAction* m_connectAction;
    class QAction* m_disconnectAction;
};
