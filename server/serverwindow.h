#pragma once

#include <QMainWindow>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QListWidget;
class QLabel;
class QCheckBox;
class FTPServer;

// 服务端管理窗口:启停服务、根目录/端口配置、用户增删、会话与传输日志。
class ServerWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit ServerWindow(QWidget* parent = nullptr);
    ~ServerWindow() override;

private slots:
    void onBrowseRootPath();
    void onToggleServer();
    void onAddUser();
    void onRemoveUser();
    void onToggleDiscovery();

    void onLogMessage(const QString& message);
    void onClientConnected(const QString& address);
    void onClientDisconnected(const QString& address);
    void onTransferStarted(const QString& id, const QString& fileName, bool isUpload);
    void onTransferProgress(const QString& id, qint64 bytesTransferred, qint64 totalBytes);
    void onTransferCompleted(const QString& id, const QString& fileName);

private:
    void loadSettings();
    void saveUserSettings();
    void appendLog(const QString& text);
    void updateStatusLabel();
    void addUserRow(const QString& username);

    FTPServer* m_server;

    QLineEdit* m_rootPathEdit;
    QSpinBox* m_portSpin;
    QPushButton* m_startStopButton;
    QCheckBox* m_discoveryCheck;

    QTableWidget* m_usersTable;
    QLineEdit* m_newUsernameEdit;
    QLineEdit* m_newPasswordEdit;

    QListWidget* m_logView;
    QLabel* m_statusLabel;
};
