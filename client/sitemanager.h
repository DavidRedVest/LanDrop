#pragma once

#include <QDialog>
#include <QString>
#include <QList>

#include "../common/ftptypes.h"

#include <memory>

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QSpinBox;
class QCheckBox;

namespace core {
class DiscoveryListener;
} // namespace core

// 局域网发现面板展示用的一条记录,和 core::DiscoveredDevice 内容对应但是纯 Qt
// 类型——sitemanager.h 不该为了这一个小结构体去 #include core/ 的头文件。
struct DiscoveredDeviceInfo {
    QString deviceName;
    QString address;
    quint16 servicePort = 0;
};

// 保存的一个站点连接信息(如"Mac"/"Win11")
struct SiteInfo {
    QString name;
    QString host;
    quint16 port = FTP::DEFAULT_PORT;
    QString username;
    // 仅当 rememberPassword 为 true 时才有效并持久化——注意:phase 1 是明文存进
    // QSettings(本机配置文件),没有接入系统钥匙串,只适合个人局域网设备使用。
    QString password;
    bool rememberPassword = false;
};

// 站点管理对话框:增删改站点、选中后点击"连接"关闭对话框并返回选中站点。
class SiteManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit SiteManagerDialog(QWidget* parent = nullptr);
    ~SiteManagerDialog() override;

    static QList<SiteInfo> loadSites();
    static void saveSites(const QList<SiteInfo>& sites);

    SiteInfo selectedSite() const { return m_selected; }

private slots:
    void onSiteRowChanged(int row);
    void onNew();
    void onSave();
    void onDelete();
    void onConnect();
    void onDiscoveredDeviceActivated(QListWidgetItem* item);

private:
    void refreshList();
    void loadFormFromSite(const SiteInfo& site);
    SiteInfo formToSite() const;
    // 跑在 UI 线程上,由 core::DiscoveryListener 的回调通过
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection) 调用过来。
    void applyDiscoveredDevices(const QList<DiscoveredDeviceInfo>& devices);

    QList<SiteInfo> m_sites;
    SiteInfo m_selected;
    int m_currentIndex = -1;

    QListWidget* m_listWidget;
    QLineEdit* m_nameEdit;
    QLineEdit* m_hostEdit;
    QSpinBox* m_portSpin;
    QLineEdit* m_usernameEdit;
    QLineEdit* m_passwordEdit;
    QCheckBox* m_rememberCheck;

    // 局域网发现:对话框打开期间自动扫描,双击列表里的一项把地址/端口自动填进
    // 表单(账号密码仍需手动填,广播包里不带凭据)。关闭对话框时自动停止扫描。
    QListWidget* m_discoveredListWidget;
    QList<DiscoveredDeviceInfo> m_discoveredDevices; // 和 m_discoveredListWidget 的行一一对应
    std::unique_ptr<core::DiscoveryListener> m_discoveryListener;
};
