#pragma once

#include <QMainWindow>

class QLabel;
class Connection;
class TransferQueue;
class FileBrowserPanel;
class TransferWidget;
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

private:
    void connectToSite(const SiteInfo& site);
    void setConnectedUiState(bool connected);

    Connection* m_connection;
    TransferQueue* m_transferQueue;
    FileBrowserPanel* m_localPanel;
    FileBrowserPanel* m_remotePanel;
    TransferWidget* m_transferWidget;
    QLabel* m_statusLabel;

    QString m_pendingUsername;
    QString m_pendingPassword;

    class QAction* m_connectAction;
    class QAction* m_disconnectAction;
};
