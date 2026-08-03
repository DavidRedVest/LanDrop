// Headless Qt 集成测试:直接链接 client/connection.cpp、client/transfer.cpp、
// server/ftpserver.cpp(它们不依赖 QWidget),驱动一个真实 FTPServer 和一个真实
// Connection + TransferQueue 在 127.0.0.1 上真实互通——延续 CLAUDE.md 里记录的
// 本项目一贯验证方法论("write a small headless Qt console program... drive a
// real FTPServer + Connection + TransferQueue against each other")。这次是
// 阶段 G(GUI 接入新 core 核心)之后的版本,不是旧自定义协议那次。
//
// 之所以用这个而不是继续用 macOS Accessibility UI 自动化跑真实点击:重复触发了
// 一个已知的、和这次改动无关的 Qt 6.8 + macOS 无障碍功能框架崩溃(libqcocoa.dylib
// 内部,CLAUDE.md 里也记录过同一类崩溃)——这个 headless 路径能确定性地覆盖同一套
// GUI 代码(Connection/TransferQueue/FTPServer 的真实实现,不是 mock),不依赖
// 脆弱的 UI 自动化工具链。
#include "../client/connection.h"
#include "../client/transfer.h"
#include "../server/ftpserver.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <functional>
#include <iostream>

namespace {
int g_failures = 0;

void check(bool condition, const QString& what) {
    if (condition) {
        std::cout << "[OK] " << what.toStdString() << std::endl;
    } else {
        std::cout << "[FAIL] " << what.toStdString() << std::endl;
        ++g_failures;
    }
}

// 没有真正的用户界面,但要真的转起一个 Qt 事件循环——这是 headless 测试驱动异步
// 信号/槽(包括跨线程 QueuedConnection 投递的那些)的标准做法。
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs) {
    if (predicate()) return true;
    QEventLoop loop;
    QTimer pollTimer;
    QObject::connect(&pollTimer, &QTimer::timeout, [&] {
        if (predicate()) loop.quit();
    });
    pollTimer.start(20);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return predicate();
}

QByteArray sha256OfFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QString serverRoot = QDir::tempPath() + "/landrop_gui_integration_test_root";
    const QString downloadDir = QDir::tempPath() + "/landrop_gui_integration_test_download";
    QDir(serverRoot).removeRecursively();
    QDir(downloadDir).removeRecursively();
    QDir().mkpath(serverRoot);
    QDir().mkpath(downloadDir);

    FTPServer server;
    server.setRootPath(serverRoot);
    server.addUser("guiuser", "guipass");
    constexpr quint16 kPort = 19590;
    check(server.start(kPort), "FTPServer::start (Qt wrapper over core::FtpServer) binds a real port");

    // ---- Connection: 连接 + 登录 + 目录列表往返 ----
    Connection connection;
    bool connectedFired = false;
    QObject::connect(&connection, &Connection::connected, [&] { connectedFired = true; });
    QString connectionError;
    QObject::connect(&connection, &Connection::connectionError, [&](const QString& msg) { connectionError = msg; });

    connection.connectToHost("127.0.0.1", kPort);
    check(waitUntil([&] { return connectedFired; }, 5000),
          "Connection::connected fires (background std::thread + core::FtpClient::connect succeeded, error: " +
              connectionError + ")");

    bool loginFired = false;
    bool loginOk = false;
    QObject::connect(&connection, &Connection::loginResult, [&](bool success, const QString&) {
        loginOk = success;
        loginFired = true;
    });
    connection.login("guiuser", "guipass");
    check(waitUntil([&] { return loginFired; }, 5000), "Connection::loginResult fires");
    check(loginOk, "login succeeds with correct credentials via the new Connection/core::FtpClient path");

    bool listFired = false;
    bool listOk = false;
    QObject::connect(&connection, &Connection::directoryListed,
                      [&](bool success, const QString&, const QString&, const QList<FTP::FileInfo>&) {
                          listOk = success;
                          listFired = true;
                      });
    connection.listDirectory("/");
    check(waitUntil([&] { return listFired; }, 5000), "Connection::directoryListed fires");
    check(listOk, "LIST / succeeds via the new Connection/core::FtpClient path");

    // ---- mkdir/rename/delete 往返,验证 operationResult 信号 ----
    bool mkdirFired = false;
    bool mkdirOk = false;
    QObject::connect(&connection, &Connection::operationResult, [&](bool success, const QString&) {
        mkdirOk = success;
        mkdirFired = true;
    });
    connection.mkdir("/sub");
    check(waitUntil([&] { return mkdirFired; }, 5000), "Connection::operationResult fires for MKD");
    check(mkdirOk, "MKD /sub succeeds");
    check(QDir(serverRoot + "/sub").exists(), "MKD actually created the directory on server disk");

    // ---- TransferQueue: 并发上传+下载往返,字节级 SHA-256 校验 ----
    TransferQueue queue;
    queue.setConnectionInfo("127.0.0.1", kPort, "guiuser", "guipass");

    const QString localSrc = QDir::tempPath() + "/landrop_gui_integration_upload_src.bin";
    {
        QFile f(localSrc);
        check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "create local test upload source file");
        QByteArray data;
        data.resize(2 * 1024 * 1024); // 2MB,确保多个 256KB 分块
        for (int i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 13 + 7) % 251);
        f.write(data);
    }
    const QByteArray srcHash = sha256OfFile(localSrc);
    check(!srcHash.isEmpty(), "computed source file SHA-256 for later comparison");

    queue.enqueueUpload(localSrc, "/");
    check(waitUntil(
              [&] {
                  return queue.rowCount() > 0 && queue.data(queue.index(0, 0), TransferQueue::StateRole).toInt() ==
                                                      static_cast<int>(TransferTask::State::Completed);
              },
              15000),
          "TransferQueue upload row reaches Completed state");

    const QString remotePath = "/landrop_gui_integration_upload_src.bin";
    const QString serverSidePath = serverRoot + remotePath;
    check(QFile::exists(serverSidePath), "uploaded file actually exists on server disk");
    check(sha256OfFile(serverSidePath) == srcHash, "uploaded file content matches source byte-for-byte (SHA-256)");

    const qint64 remoteSize = QFileInfo(serverSidePath).size();
    queue.enqueueDownload(remotePath, downloadDir, remoteSize);
    check(waitUntil(
              [&] {
                  return queue.rowCount() > 1 && queue.data(queue.index(1, 0), TransferQueue::StateRole).toInt() ==
                                                      static_cast<int>(TransferTask::State::Completed);
              },
              15000),
          "TransferQueue download row reaches Completed state");

    const QString downloadedPath = downloadDir + "/landrop_gui_integration_upload_src.bin";
    check(sha256OfFile(downloadedPath) == srcHash,
          "downloaded file content matches original source byte-for-byte (SHA-256)");

    connection.disconnectFromHost();
    server.stop();
    check(!server.isRunning(), "FTPServer::stop() actually stops the server");

    QDir(serverRoot).removeRecursively();
    QDir(downloadDir).removeRecursively();
    QFile::remove(localSrc);

    if (g_failures == 0) {
        std::cout << "=== ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << g_failures << " FAILURE(S) ===" << std::endl;
    return 1;
}
