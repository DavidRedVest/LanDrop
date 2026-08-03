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
#include "../client/foldertransfer.h"
#include "../server/ftpserver.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
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

    // ---- 错误密码时,loginResult 的 message 必须是服务器的真实回复文本
    // (core::FtpClient 通过 ErrorCallback 报出来的原文,比如 "login failed:
    // Login incorrect"),不能是硬编码的通用提示——之前 Connection 完全没接
    // core::FtpClient::setErrorCallback(),所有失败场景在 UI 上只能看到一句
    // 看不出真实原因的固定文案,这个回归测试就是防止这个接线以后又被悄悄去掉。 ----
    {
        Connection badConnection;
        bool badConnectedFired = false;
        QObject::connect(&badConnection, &Connection::connected, [&] { badConnectedFired = true; });
        badConnection.connectToHost("127.0.0.1", kPort);
        check(waitUntil([&] { return badConnectedFired; }, 5000), "second Connection connects for bad-login test");

        bool badLoginFired = false;
        bool badLoginOk = true;
        QString badLoginMessage;
        QObject::connect(&badConnection, &Connection::loginResult, [&](bool success, const QString& message) {
            badLoginOk = success;
            badLoginMessage = message;
            badLoginFired = true;
        });
        badConnection.login("guiuser", "wrongpassword");
        check(waitUntil([&] { return badLoginFired; }, 5000), "loginResult fires for wrong password");
        check(!badLoginOk, "login with wrong password is rejected");
        check(!badLoginMessage.isEmpty() && badLoginMessage.contains("Login incorrect"),
              "loginResult carries the real server-provided error text, not a generic hardcoded message (got '" +
                  badLoginMessage + "')");
        badConnection.disconnectFromHost();
    }

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

    // ---- 暂停/续传/取消:通过 TransferQueue 这一层(pauseRow/resumeRow/cancelRow),
    // 不是直接调 FtpTransferManager——阶段 E 已经在 core 层面验证过暂停续传机制
    // 本身是对的,这里要确认的是 Qt 适配层(行号查找、状态回调映射)接得对。----
    const QString pauseSrc = QDir::tempPath() + "/landrop_gui_integration_pause_src.bin";
    {
        QFile f(pauseSrc);
        check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "create local pause-test source file");
        QByteArray data;
        data.resize(100 * 1024 * 1024); // 100MB:确保在 200ms 节流窗口内不会瞬间传完
        for (int i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 5 + 3) % 251);
        f.write(data);
    }
    const QByteArray pauseSrcHash = sha256OfFile(pauseSrc);

    queue.enqueueUpload(pauseSrc, "/");
    const int pauseRow = queue.rowCount() - 1;
    check(pauseRow >= 0, "pause-test task got a row in the model");

    check(waitUntil(
              [&] { return queue.data(queue.index(pauseRow, 0), TransferQueue::BytesTransferredRole).toLongLong() > 0; },
              5000),
          "pause-test upload starts actually transferring bytes (bytesTransferred > 0)");

    queue.pauseRow(pauseRow);
    const bool reachedPaused = waitUntil(
        [&] {
            return queue.data(queue.index(pauseRow, 0), TransferQueue::StateRole).toInt() ==
                   static_cast<int>(TransferTask::State::Paused);
        },
        5000);
    const qint64 stateAfterPauseAttempt = queue.data(queue.index(pauseRow, 0), TransferQueue::StateRole).toInt();
    // 极端情况下(比如这台机器的回环速度快到 100MB 在节流窗口内就传完了),pauseRow()
    // 可能"扑了个空"——任务已经是 Completed 了,这不是 bug,只是没抓住暂停窗口。
    // 只有当它既没到 Paused、也没到 Completed 时才是真的有问题。
    check(reachedPaused || stateAfterPauseAttempt == static_cast<int>(TransferTask::State::Completed),
          "pauseRow() either paused the task or it had already completed (not stuck in another state)");

    if (reachedPaused) {
        const qint64 bytesAtPause =
            queue.data(queue.index(pauseRow, 0), TransferQueue::BytesTransferredRole).toLongLong();
        check(bytesAtPause > 0 && bytesAtPause < 100 * 1024 * 1024,
              "paused task has partial bytesTransferred (got " + QString::number(bytesAtPause) + ")");

        queue.resumeRow(pauseRow);
        check(waitUntil(
                  [&] {
                      return queue.data(queue.index(pauseRow, 0), TransferQueue::StateRole).toInt() ==
                             static_cast<int>(TransferTask::State::Completed);
                  },
                  20000),
              "resumeRow() lets the task reach Completed");

        const QString pauseServerPath = serverRoot + "/landrop_gui_integration_pause_src.bin";
        check(sha256OfFile(pauseServerPath) == pauseSrcHash,
              "resumed upload's final content matches original source byte-for-byte (SHA-256)");
    }

    // 取消一个还在排队、尚未被任何 worker 处理的任务——确定性分支,不涉及网络时序。
    // 前提是这个任务真的还 Queued 着:TransferQueue 内部池子有 3 个 worker,这时候
    // 前面几个任务都已经跑完、worker 都闲着,如果只排一个小文件任务,大概率会在
    // cancelRow() 真正执行前就被某个空闲 worker 抢走——所以先排几个足够大的"占位"
    // 任务把 3 个 worker 都占满,确定性地保证紧接着排的这个小任务只能留在队列里。
    QStringList busySrcPaths;
    for (int i = 0; i < 3; ++i) {
        const QString busyPath = QDir::tempPath() + QStringLiteral("/landrop_gui_integration_busy_src_%1.bin").arg(i);
        {
            QFile f(busyPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            QByteArray data;
            data.resize(80 * 1024 * 1024); // 80MB,确保占用 worker 的时间够长
            f.write(data);
        }
        busySrcPaths.append(busyPath);
        queue.enqueueUpload(busyPath, "/");
    }

    const QString cancelSrc = QDir::tempPath() + "/landrop_gui_integration_cancel_src.bin";
    {
        QFile f(cancelSrc);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(QByteArray(1024, 'x'));
    }
    queue.enqueueUpload(cancelSrc, "/");
    const int cancelRow = queue.rowCount() - 1;
    queue.cancelRow(cancelRow);
    check(waitUntil(
              [&] {
                  return queue.data(queue.index(cancelRow, 0), TransferQueue::StateRole).toInt() ==
                         static_cast<int>(TransferTask::State::Cancelled);
              },
              5000),
          "cancelRow() transitions the task to Cancelled");

    QFile::remove(pauseSrc);
    QFile::remove(cancelSrc);
    for (const QString& busyPath : busySrcPaths) QFile::remove(busyPath);

    // ---- FolderTransferCoordinator: 递归文件夹上传/下载往返 ----
    // 本地目录结构:
    //   <folderSrc>/topfolder/file_a.bin
    //   <folderSrc>/topfolder/sub/file_b.bin
    // 期望上传后服务端出现 /topfolder/file_a.bin 和 /topfolder/sub/file_b.bin,
    // 嵌套结构原样保留,不是被拍平。
    FolderTransferCoordinator coordinator(&connection, &queue);

    const QString folderSrcBase = QDir::tempPath() + "/landrop_gui_integration_folder_src";
    QDir(folderSrcBase).removeRecursively();
    const QString topFolder = folderSrcBase + "/topfolder";
    QDir().mkpath(topFolder + "/sub");
    const QByteArray fileAData = QByteArray("file-a-content").repeated(1000);
    const QByteArray fileBData = QByteArray("file-b-nested-content").repeated(1000);
    {
        QFile fa(topFolder + "/file_a.bin");
        fa.open(QIODevice::WriteOnly | QIODevice::Truncate);
        fa.write(fileAData);
        QFile fb(topFolder + "/sub/file_b.bin");
        fb.open(QIODevice::WriteOnly | QIODevice::Truncate);
        fb.write(fileBData);
    }

    coordinator.uploadFolders({topFolder}, "/");

    const QString serverFileA = serverRoot + "/topfolder/file_a.bin";
    const QString serverFileB = serverRoot + "/topfolder/sub/file_b.bin";
    check(waitUntil([&] { return QFile::exists(serverFileA) && QFile::exists(serverFileB); }, 15000),
          "folder upload recreates the nested directory structure on the server disk");
    check(waitUntil([&] { return sha256OfFile(serverFileA) == QCryptographicHash::hash(fileAData, QCryptographicHash::Sha256); },
                     15000),
          "uploaded top-level file in the folder matches source content byte-for-byte");
    check(waitUntil([&] { return sha256OfFile(serverFileB) == QCryptographicHash::hash(fileBData, QCryptographicHash::Sha256); },
                     15000),
          "uploaded nested file in the folder matches source content byte-for-byte");

    const QString folderDownloadBase = QDir::tempPath() + "/landrop_gui_integration_folder_download";
    QDir(folderDownloadBase).removeRecursively();
    QDir().mkpath(folderDownloadBase);

    coordinator.downloadFolders({"/topfolder"}, folderDownloadBase);

    const QString downloadedFileA = folderDownloadBase + "/topfolder/file_a.bin";
    const QString downloadedFileB = folderDownloadBase + "/topfolder/sub/file_b.bin";
    check(waitUntil([&] { return QFile::exists(downloadedFileA) && QFile::exists(downloadedFileB); }, 15000),
          "folder download recreates the nested directory structure locally");
    check(waitUntil([&] { return sha256OfFile(downloadedFileA) == sha256OfFile(serverFileA); }, 15000),
          "downloaded top-level file matches the server copy byte-for-byte");
    check(waitUntil([&] { return sha256OfFile(downloadedFileB) == sha256OfFile(serverFileB); }, 15000),
          "downloaded nested file matches the server copy byte-for-byte");

    QDir(folderSrcBase).removeRecursively();
    QDir(folderDownloadBase).removeRecursively();

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
