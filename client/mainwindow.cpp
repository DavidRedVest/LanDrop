#include "mainwindow.h"
#include "connection.h"
#include "transfer.h"
#include "filebrowser.h"
#include "transferwidget.h"
#include "sitemanager.h"
#include "foldertransfer.h"

#include <QSplitter>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFont>
#include <QAction>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_connection(new Connection(this))
    , m_transferQueue(new TransferQueue(this))
    , m_folderTransfer(new FolderTransferCoordinator(m_connection, m_transferQueue, this))
{
    setWindowTitle(QStringLiteral("LanDrop 客户端"));
    resize(1100, 700);

    m_localPanel = new FileBrowserPanel(BrowserMode::Local, this);
    m_remotePanel = new FileBrowserPanel(BrowserMode::Remote, this);
    m_remotePanel->setConnection(m_connection);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_localPanel);
    splitter->addWidget(m_remotePanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    m_transferWidget = new TransferWidget(m_transferQueue, this);
    auto* dock = new QDockWidget(QStringLiteral("传输队列"), this);
    dock->setWidget(m_transferWidget);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    m_statusLabel = new QLabel(QStringLiteral("未连接"), this);
    statusBar()->addWidget(m_statusLabel);

    m_connectAction = new QAction(QStringLiteral("连接..."), this);
    connect(m_connectAction, &QAction::triggered, this, &MainWindow::onOpenSiteManager);
    m_disconnectAction = new QAction(QStringLiteral("断开连接"), this);
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::onDisconnectClicked);
    m_disconnectAction->setEnabled(false);

    auto* siteMenu = menuBar()->addMenu(QStringLiteral("站点"));
    siteMenu->addAction(m_connectAction);
    siteMenu->addAction(m_disconnectAction);

    // 连接入口除了藏在菜单栏(macOS 上菜单栏在屏幕顶端、不在窗口内,很容易被忽略)
    // 之外,还要在窗口本身放一个显眼的工具栏按钮,否则用户根本找不到怎么连接。
    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    QFont toolbarFont = toolbar->font();
    toolbarFont.setBold(true);
    toolbar->setFont(toolbarFont);
    toolbar->addAction(m_connectAction);
    toolbar->addAction(m_disconnectAction);

    connect(m_connection, &Connection::connected, this, &MainWindow::onConnected);
    connect(m_connection, &Connection::disconnected, this, &MainWindow::onDisconnected);
    connect(m_connection, &Connection::connectionError, this, &MainWindow::onConnectionError);
    connect(m_connection, &Connection::loginResult, this, &MainWindow::onLoginResult);

    connect(m_localPanel, &FileBrowserPanel::uploadRequested, this, &MainWindow::onLocalUploadRequested);
    connect(m_remotePanel, &FileBrowserPanel::downloadRequested, this, &MainWindow::onRemoteDownloadRequested);
    connect(m_localPanel, &FileBrowserPanel::statusMessage, this, &MainWindow::onStatusMessage);
    connect(m_remotePanel, &FileBrowserPanel::statusMessage, this, &MainWindow::onStatusMessage);

    connect(m_localPanel, &FileBrowserPanel::folderUploadRequested, this, &MainWindow::onFolderUploadRequested);
    connect(m_remotePanel, &FileBrowserPanel::folderDownloadRequested, this, &MainWindow::onFolderDownloadRequested);

    setConnectedUiState(false);
}

void MainWindow::connectToSite(const SiteInfo& site) {
    m_pendingUsername = site.username;
    m_pendingPassword = site.password;
    m_statusLabel->setText(QStringLiteral("正在连接 %1:%2 ...").arg(site.host).arg(site.port));
    m_connection->connectToHost(site.host, site.port);
}

void MainWindow::onOpenSiteManager() {
    SiteManagerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    const SiteInfo site = dialog.selectedSite();
    if (site.host.isEmpty()) return;

    // "记住密码" 只控制是否把密码持久化到 QSettings,不影响本次连接是否使用
    // 刚刚在对话框里输入的密码——之前这里误把"没有勾记住密码"也当成"密码没填",
    // 导致用户明明填了密码,点连接后还会被多问一次。
    QString password = site.password;
    if (password.isEmpty()) {
        bool ok = false;
        password = QInputDialog::getText(this, QStringLiteral("登录"),
                                          QStringLiteral("密码(用户: %1):").arg(site.username),
                                          QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }

    SiteInfo effective = site;
    effective.password = password;
    connectToSite(effective);
}

void MainWindow::onDisconnectClicked() {
    m_connection->disconnectFromHost();
}

void MainWindow::onConnected() {
    m_connection->login(m_pendingUsername, m_pendingPassword);
}

void MainWindow::onDisconnected() {
    m_statusLabel->setText(QStringLiteral("未连接"));
    setConnectedUiState(false);
}

void MainWindow::onConnectionError(const QString& message) {
    QMessageBox::warning(this, QStringLiteral("连接错误"), message);
    m_statusLabel->setText(QStringLiteral("连接错误: %1").arg(message));
    setConnectedUiState(false);
}

void MainWindow::onLoginResult(bool success, const QString& message) {
    if (!success) {
        QMessageBox::warning(this, QStringLiteral("登录失败"), message);
        m_connection->disconnectFromHost();
        return;
    }
    m_statusLabel->setText(QStringLiteral("已连接: %1@%2").arg(m_pendingUsername, m_connection->host()));
    setConnectedUiState(true);
    // 传输队列不再借用 Connection 的控制通道申请令牌,它自己维护一个独立的 FTP
    // 连接池,需要单独告诉它连去哪、用什么账号——见 client/transfer.h 的说明。
    m_transferQueue->setConnectionInfo(m_connection->host(), m_connection->controlPort(), m_pendingUsername,
                                        m_pendingPassword);
    m_remotePanel->navigateTo("/");
}

void MainWindow::onLocalUploadRequested(const QStringList& localPaths) {
    if (!m_connection->isConnected()) {
        QMessageBox::information(this, QStringLiteral("上传"), QStringLiteral("请先连接到服务器。"));
        return;
    }
    for (const QString& path : localPaths) {
        m_transferQueue->enqueueUpload(path, m_remotePanel->currentPath());
    }
}

void MainWindow::onRemoteDownloadRequested(const QList<QPair<QString, qint64>>& remoteFiles) {
    for (const auto& file : remoteFiles) {
        m_transferQueue->enqueueDownload(file.first, m_localPanel->currentPath(), file.second);
    }
}

void MainWindow::onFolderUploadRequested(const QStringList& localFolderPaths) {
    if (!m_connection->isConnected()) {
        QMessageBox::information(this, QStringLiteral("上传"), QStringLiteral("请先连接到服务器。"));
        return;
    }
    m_folderTransfer->uploadFolders(localFolderPaths, m_remotePanel->currentPath());
}

void MainWindow::onFolderDownloadRequested(const QStringList& remoteFolderPaths) {
    m_folderTransfer->downloadFolders(remoteFolderPaths, m_localPanel->currentPath());
}

void MainWindow::onStatusMessage(const QString& message) {
    statusBar()->showMessage(message, 5000);
}

void MainWindow::setConnectedUiState(bool connected) {
    m_disconnectAction->setEnabled(connected);
}
