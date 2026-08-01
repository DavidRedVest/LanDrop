#include "serverwindow.h"
#include "ftpserver.h"
#include "../common/protocol.h"

#include <QWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QAbstractItemView>
#include <QListWidget>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QHeaderView>
#include <QDateTime>

ServerWindow::ServerWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_server(new FTPServer(this))
{
    setWindowTitle(QStringLiteral("LanDrop 服务端"));
    resize(720, 560);

    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);

    // 根目录 + 端口 + 启停
    auto* configGroup = new QGroupBox(QStringLiteral("服务配置"), central);
    auto* configLayout = new QFormLayout(configGroup);

    auto* rootRow = new QWidget(configGroup);
    auto* rootRowLayout = new QHBoxLayout(rootRow);
    rootRowLayout->setContentsMargins(0, 0, 0, 0);
    m_rootPathEdit = new QLineEdit(m_server->rootPath(), rootRow);
    auto* browseButton = new QPushButton(QStringLiteral("浏览..."), rootRow);
    rootRowLayout->addWidget(m_rootPathEdit);
    rootRowLayout->addWidget(browseButton);
    configLayout->addRow(QStringLiteral("根目录:"), rootRow);

    m_portSpin = new QSpinBox(configGroup);
    m_portSpin->setRange(1024, 65534);
    m_portSpin->setValue(FTP::DEFAULT_PORT);
    configLayout->addRow(QStringLiteral("控制端口(数据端口=端口+1):"), m_portSpin);

    m_startStopButton = new QPushButton(QStringLiteral("启动服务"), configGroup);
    configLayout->addRow(m_startStopButton);

    m_statusLabel = new QLabel(QStringLiteral("未运行"), configGroup);
    configLayout->addRow(QStringLiteral("状态:"), m_statusLabel);

    mainLayout->addWidget(configGroup);

    // 用户管理
    auto* usersGroup = new QGroupBox(QStringLiteral("用户"), central);
    auto* usersLayout = new QVBoxLayout(usersGroup);

    m_usersTable = new QTableWidget(0, 2, usersGroup);
    m_usersTable->setHorizontalHeaderLabels({QStringLiteral("用户名"), QStringLiteral("操作")});
    m_usersTable->horizontalHeader()->setStretchLastSection(false);
    m_usersTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_usersTable->verticalHeader()->setVisible(false);
    m_usersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_usersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    usersLayout->addWidget(m_usersTable);

    auto* addUserRow = new QWidget(usersGroup);
    auto* addUserLayout = new QHBoxLayout(addUserRow);
    addUserLayout->setContentsMargins(0, 0, 0, 0);
    m_newUsernameEdit = new QLineEdit(addUserRow);
    m_newUsernameEdit->setPlaceholderText(QStringLiteral("用户名"));
    m_newPasswordEdit = new QLineEdit(addUserRow);
    m_newPasswordEdit->setPlaceholderText(QStringLiteral("密码"));
    m_newPasswordEdit->setEchoMode(QLineEdit::Password);
    auto* addUserButton = new QPushButton(QStringLiteral("添加用户"), addUserRow);
    addUserLayout->addWidget(m_newUsernameEdit);
    addUserLayout->addWidget(m_newPasswordEdit);
    addUserLayout->addWidget(addUserButton);
    usersLayout->addWidget(addUserRow);

    mainLayout->addWidget(usersGroup);

    // 日志
    auto* logGroup = new QGroupBox(QStringLiteral("会话 / 传输日志"), central);
    auto* logLayout = new QVBoxLayout(logGroup);
    m_logView = new QListWidget(logGroup);
    logLayout->addWidget(m_logView);
    mainLayout->addWidget(logGroup, 1);

    setCentralWidget(central);

    connect(browseButton, &QPushButton::clicked, this, &ServerWindow::onBrowseRootPath);
    connect(m_startStopButton, &QPushButton::clicked, this, &ServerWindow::onToggleServer);
    connect(addUserButton, &QPushButton::clicked, this, &ServerWindow::onAddUser);

    connect(m_server, &FTPServer::logMessage, this, &ServerWindow::onLogMessage);
    connect(m_server, &FTPServer::clientConnected, this, &ServerWindow::onClientConnected);
    connect(m_server, &FTPServer::clientDisconnected, this, &ServerWindow::onClientDisconnected);
    connect(m_server, &FTPServer::transferStarted, this, &ServerWindow::onTransferStarted);
    connect(m_server, &FTPServer::transferProgress, this, &ServerWindow::onTransferProgress);
    connect(m_server, &FTPServer::transferCompleted, this, &ServerWindow::onTransferCompleted);

    loadSettings();
}

ServerWindow::~ServerWindow() {
    QSettings settings;
    settings.setValue("server/rootPath", m_rootPathEdit->text());
    settings.setValue("server/port", m_portSpin->value());
}

void ServerWindow::loadSettings() {
    QSettings settings;
    m_rootPathEdit->setText(settings.value("server/rootPath", m_server->rootPath()).toString());
    m_portSpin->setValue(settings.value("server/port", FTP::DEFAULT_PORT).toInt());

    const int count = settings.beginReadArray("server/users");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString username = settings.value("username").toString();
        const QString hash = settings.value("passwordHash").toString();
        if (username.isEmpty() || hash.isEmpty()) continue;
        m_server->addUserWithHash(username, hash);
        addUserRow(username);
    }
    settings.endArray();
}

void ServerWindow::saveUserSettings() {
    QSettings settings;
    const QMap<QString, QString> users = m_server->userHashes();
    settings.beginWriteArray("server/users");
    int i = 0;
    for (auto it = users.constBegin(); it != users.constEnd(); ++it, ++i) {
        settings.setArrayIndex(i);
        settings.setValue("username", it.key());
        settings.setValue("passwordHash", it.value());
    }
    settings.endArray();
}

void ServerWindow::addUserRow(const QString& username) {
    const int row = m_usersTable->rowCount();
    m_usersTable->insertRow(row);
    m_usersTable->setItem(row, 0, new QTableWidgetItem(username));
    auto* removeButton = new QPushButton(QStringLiteral("删除"));
    connect(removeButton, &QPushButton::clicked, this, &ServerWindow::onRemoveUser);
    m_usersTable->setCellWidget(row, 1, removeButton);
}

void ServerWindow::onBrowseRootPath() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择根目录"), m_rootPathEdit->text());
    if (!dir.isEmpty()) {
        m_rootPathEdit->setText(dir);
    }
}

void ServerWindow::onToggleServer() {
    if (m_server->isRunning()) {
        m_server->stop();
        m_startStopButton->setText(QStringLiteral("启动服务"));
        m_rootPathEdit->setEnabled(true);
        m_portSpin->setEnabled(true);
    } else {
        m_server->setRootPath(m_rootPathEdit->text());
        if (!m_server->start(static_cast<quint16>(m_portSpin->value()))) {
            QMessageBox::warning(this, QStringLiteral("启动失败"), QStringLiteral("端口可能已被占用,请更换端口重试。"));
            return;
        }
        m_startStopButton->setText(QStringLiteral("停止服务"));
        m_rootPathEdit->setEnabled(false);
        m_portSpin->setEnabled(false);
    }
    updateStatusLabel();
}

void ServerWindow::onAddUser() {
    const QString username = m_newUsernameEdit->text().trimmed();
    const QString password = m_newPasswordEdit->text();
    if (username.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("添加用户"), QStringLiteral("用户名和密码不能为空。"));
        return;
    }
    m_server->addUser(username, password);
    addUserRow(username);
    saveUserSettings();
    m_newUsernameEdit->clear();
    m_newPasswordEdit->clear();
}

void ServerWindow::onRemoveUser() {
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) return;

    for (int row = 0; row < m_usersTable->rowCount(); ++row) {
        if (m_usersTable->cellWidget(row, 1) == button) {
            const QString username = m_usersTable->item(row, 0)->text();
            m_server->removeUser(username);
            m_usersTable->removeRow(row);
            saveUserSettings();
            break;
        }
    }
}

void ServerWindow::appendLog(const QString& text) {
    m_logView->addItem(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("hh:mm:ss"), text));
    m_logView->scrollToBottom();
}

void ServerWindow::updateStatusLabel() {
    if (m_server->isRunning()) {
        m_statusLabel->setText(QStringLiteral("运行中 · 端口 %1 · 当前会话 %2")
                                    .arg(m_server->serverPort())
                                    .arg(m_server->activeSessionCount()));
    } else {
        m_statusLabel->setText(QStringLiteral("未运行"));
    }
}

void ServerWindow::onLogMessage(const QString& message) {
    appendLog(message);
}

void ServerWindow::onClientConnected(const QString& address) {
    appendLog(QStringLiteral("客户端连接: %1").arg(address));
    updateStatusLabel();
}

void ServerWindow::onClientDisconnected(const QString& address) {
    appendLog(QStringLiteral("客户端断开: %1").arg(address));
    updateStatusLabel();
}

void ServerWindow::onTransferStarted(const QString& id, const QString& fileName, bool isUpload) {
    Q_UNUSED(id);
    appendLog(QStringLiteral("%1 开始: %2").arg(isUpload ? QStringLiteral("上传") : QStringLiteral("下载"), fileName));
}

void ServerWindow::onTransferProgress(const QString& id, qint64 bytesTransferred, qint64 totalBytes) {
    Q_UNUSED(id);
    Q_UNUSED(bytesTransferred);
    Q_UNUSED(totalBytes);
    // 阶段1:服务端窗口只展示关键事件,不逐字节刷新日志,避免刷屏
}

void ServerWindow::onTransferCompleted(const QString& id, const QString& fileName) {
    Q_UNUSED(id);
    appendLog(QStringLiteral("传输完成: %1").arg(fileName));
}
