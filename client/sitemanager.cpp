#include "sitemanager.h"

#include <QListWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QSettings>
#include <QMessageBox>

QList<SiteInfo> SiteManagerDialog::loadSites() {
    QSettings settings;
    QList<SiteInfo> sites;
    const int count = settings.beginReadArray("client/sites");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SiteInfo site;
        site.name = settings.value("name").toString();
        site.host = settings.value("host").toString();
        site.port = static_cast<quint16>(settings.value("port", FTP::DEFAULT_PORT).toInt());
        site.username = settings.value("username").toString();
        site.rememberPassword = settings.value("rememberPassword", false).toBool();
        if (site.rememberPassword) {
            site.password = settings.value("password").toString();
        }
        sites.append(site);
    }
    settings.endArray();
    return sites;
}

void SiteManagerDialog::saveSites(const QList<SiteInfo>& sites) {
    QSettings settings;
    settings.beginWriteArray("client/sites");
    for (int i = 0; i < sites.size(); ++i) {
        settings.setArrayIndex(i);
        const SiteInfo& site = sites[i];
        settings.setValue("name", site.name);
        settings.setValue("host", site.host);
        settings.setValue("port", site.port);
        settings.setValue("username", site.username);
        settings.setValue("rememberPassword", site.rememberPassword);
        settings.setValue("password", site.rememberPassword ? site.password : QString());
    }
    settings.endArray();
}

SiteManagerDialog::SiteManagerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("站点管理"));
    resize(560, 360);

    m_sites = loadSites();

    auto* mainLayout = new QHBoxLayout(this);

    m_listWidget = new QListWidget(this);
    mainLayout->addWidget(m_listWidget, 1);

    auto* rightLayout = new QVBoxLayout();
    auto* form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    m_hostEdit = new QLineEdit(this);
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65534);
    m_portSpin->setValue(FTP::DEFAULT_PORT);
    m_usernameEdit = new QLineEdit(this);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_rememberCheck = new QCheckBox(QStringLiteral("记住密码(明文保存在本机,仅建议个人设备使用)"), this);

    form->addRow(QStringLiteral("站点名称:"), m_nameEdit);
    form->addRow(QStringLiteral("主机地址:"), m_hostEdit);
    form->addRow(QStringLiteral("端口:"), m_portSpin);
    form->addRow(QStringLiteral("用户名:"), m_usernameEdit);
    form->addRow(QStringLiteral("密码:"), m_passwordEdit);
    form->addRow(m_rememberCheck);
    rightLayout->addLayout(form);

    auto* editButtonsLayout = new QHBoxLayout();
    auto* newButton = new QPushButton(QStringLiteral("新建"), this);
    auto* saveButton = new QPushButton(QStringLiteral("保存"), this);
    auto* deleteButton = new QPushButton(QStringLiteral("删除"), this);
    editButtonsLayout->addWidget(newButton);
    editButtonsLayout->addWidget(saveButton);
    editButtonsLayout->addWidget(deleteButton);
    rightLayout->addLayout(editButtonsLayout);

    rightLayout->addStretch(1);

    auto* buttonBox = new QDialogButtonBox(this);
    auto* connectButton = buttonBox->addButton(QStringLiteral("连接"), QDialogButtonBox::AcceptRole);
    buttonBox->addButton(QDialogButtonBox::Cancel);
    rightLayout->addWidget(buttonBox);

    mainLayout->addLayout(rightLayout, 2);

    connect(m_listWidget, &QListWidget::currentRowChanged, this, &SiteManagerDialog::onSiteRowChanged);
    connect(newButton, &QPushButton::clicked, this, &SiteManagerDialog::onNew);
    connect(saveButton, &QPushButton::clicked, this, &SiteManagerDialog::onSave);
    connect(deleteButton, &QPushButton::clicked, this, &SiteManagerDialog::onDelete);
    connect(connectButton, &QPushButton::clicked, this, &SiteManagerDialog::onConnect);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshList();
    if (!m_sites.isEmpty()) {
        m_listWidget->setCurrentRow(0);
    }
}

void SiteManagerDialog::refreshList() {
    m_listWidget->clear();
    for (const SiteInfo& site : m_sites) {
        m_listWidget->addItem(site.name.isEmpty() ? site.host : site.name);
    }
}

void SiteManagerDialog::loadFormFromSite(const SiteInfo& site) {
    m_nameEdit->setText(site.name);
    m_hostEdit->setText(site.host);
    m_portSpin->setValue(site.port);
    m_usernameEdit->setText(site.username);
    m_passwordEdit->setText(site.password);
    m_rememberCheck->setChecked(site.rememberPassword);
}

SiteInfo SiteManagerDialog::formToSite() const {
    SiteInfo site;
    site.name = m_nameEdit->text().trimmed();
    site.host = m_hostEdit->text().trimmed();
    site.port = static_cast<quint16>(m_portSpin->value());
    site.username = m_usernameEdit->text();
    site.password = m_passwordEdit->text();
    site.rememberPassword = m_rememberCheck->isChecked();
    return site;
}

void SiteManagerDialog::onSiteRowChanged(int row) {
    m_currentIndex = row;
    if (row >= 0 && row < m_sites.size()) {
        loadFormFromSite(m_sites[row]);
    }
}

void SiteManagerDialog::onNew() {
    m_currentIndex = -1;
    m_listWidget->setCurrentRow(-1);
    loadFormFromSite(SiteInfo{});
    m_portSpin->setValue(FTP::DEFAULT_PORT);
    m_nameEdit->setFocus();
}

void SiteManagerDialog::onSave() {
    const SiteInfo site = formToSite();
    if (site.name.isEmpty() || site.host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("保存站点"), QStringLiteral("站点名称和主机地址不能为空。"));
        return;
    }

    if (m_currentIndex >= 0 && m_currentIndex < m_sites.size()) {
        m_sites[m_currentIndex] = site;
    } else {
        m_sites.append(site);
        m_currentIndex = m_sites.size() - 1;
    }
    saveSites(m_sites);
    refreshList();
    m_listWidget->setCurrentRow(m_currentIndex);
}

void SiteManagerDialog::onDelete() {
    if (m_currentIndex < 0 || m_currentIndex >= m_sites.size()) return;
    m_sites.removeAt(m_currentIndex);
    saveSites(m_sites);
    m_currentIndex = -1;
    refreshList();
}

void SiteManagerDialog::onConnect() {
    // 始终用表单当前的值连接,而不是选中行在 m_sites 里存的旧数据——列表选择
    // 只是用来"把表单填进去"的快捷方式,用户选中一个站点后又手动改了字段
    // (没点"保存"),这时候应该按他改过的内容连,而不是悄悄连回原来保存的地址。
    const SiteInfo site = formToSite();
    if (site.host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("连接"), QStringLiteral("请先选择或填写一个站点。"));
        return;
    }
    m_selected = site;
    accept();
}
