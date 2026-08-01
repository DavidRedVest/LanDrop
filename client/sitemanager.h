#pragma once

#include <QDialog>
#include <QString>
#include <QList>

#include "../common/protocol.h"

class QListWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;

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

    static QList<SiteInfo> loadSites();
    static void saveSites(const QList<SiteInfo>& sites);

    SiteInfo selectedSite() const { return m_selected; }

private slots:
    void onSiteRowChanged(int row);
    void onNew();
    void onSave();
    void onDelete();
    void onConnect();

private:
    void refreshList();
    void loadFormFromSite(const SiteInfo& site);
    SiteInfo formToSite() const;

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
};
