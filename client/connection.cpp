#include "connection.h"

#include "../core/ftp/ftp_client.h"

#include <QMetaObject>

Connection::Connection(QObject* parent) : QObject(parent) {}

Connection::~Connection() {
    disconnectFromHost();
}

void Connection::connectToHost(const QString& host, quint16 port) {
    disconnectFromHost(); // 清理掉任何上一次连接留下的线程,再开始新的一次

    m_host = host;
    m_port = port;
    m_stopRequested = false;
    m_thread = std::thread(&Connection::threadMain, this);
}

void Connection::disconnectFromHost() {
    if (!m_thread.joinable()) return;
    m_stopRequested = true;
    m_queueCv.notify_all();
    m_thread.join();
}

void Connection::pushJob(Job job) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_jobs.push_back(std::move(job));
    }
    m_queueCv.notify_one();
}

QString Connection::takeLastErrorOr(const QString& fallback) {
    if (m_lastError.empty()) return fallback;
    QString detail = QString::fromStdString(m_lastError);
    m_lastError.clear();
    return detail;
}

void Connection::threadMain() {
    core::FtpClient client;
    // 把 core::FtpClient 内部报的真实错误文本(服务器实际回复的原文,比如
    // "530 Login incorrect"、"USER rejected: ...")接住,而不是让它被默默丢掉——
    // 之前这里完全没接 ErrorCallback,所有失败场景在 UI 上只能看到硬编码的通用
    // 中文提示,分不清到底是密码错、服务器要求 TLS、账号被限制,还是别的协议层
    // 原因。这个回调只会从这条 worker 线程自己触发(client 是这条线程私有的),
    // 不需要加锁。
    client.setErrorCallback([this](const std::string& message) { m_lastError = message; });

    const core::FtpResult result = client.connect(m_host.toStdString(), m_port, 5000);
    if (result != core::FtpResult::Ok) {
        const QString detail = takeLastErrorOr(QStringLiteral("连接失败,请检查地址、端口和网络"));
        QMetaObject::invokeMethod(
            this, [this, detail] { emit connectionError(detail); }, Qt::QueuedConnection);
        return;
    }

    m_connected = true;
    QMetaObject::invokeMethod(this, [this] { emit connected(); }, Qt::QueuedConnection);

    // 这个 try 和服务端 FtpSession::run() 里的道理完全一样:这是这条控制通道
    // 专属 std::thread 的顶层循环,任何一个 job(比如某次 login/listDirectory 的
    // lambda)里未预料到的异常逃出去,都会 std::terminate() 整个客户端进程——
    // 抓住后只结束这条连接,不影响应用其它部分。
    try {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        while (!m_stopRequested.load()) {
            m_queueCv.wait(lock, [this] { return m_stopRequested.load() || !m_jobs.empty(); });
            if (m_stopRequested.load()) break;

            Job job = std::move(m_jobs.front());
            m_jobs.pop_front();
            lock.unlock();
            job(client);
            lock.lock();
        }
    } catch (...) {
        // 吞掉,走到下面统一的断开清理逻辑。
    }

    m_connected = false;
    client.disconnect();
    QMetaObject::invokeMethod(this, [this] { emit disconnected(); }, Qt::QueuedConnection);
}

void Connection::login(const QString& username, const QString& password) {
    const std::string user = username.toStdString();
    const std::string pass = password.toStdString();
    pushJob([this, user, pass](core::FtpClient& client) {
        const bool success = client.login(user, pass) == core::FtpResult::Ok;
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("登录失败"));
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit loginResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::listDirectory(const QString& path) {
    const std::string remotePath = path.toStdString();
    pushJob([this, remotePath, path](core::FtpClient& client) {
        std::vector<core::FtpFileEntry> entries;
        const bool success = client.list(remotePath, entries) == core::FtpResult::Ok;
        QList<FTP::FileInfo> list;
        if (success) {
            list.reserve(static_cast<int>(entries.size()));
            for (const auto& e : entries) {
                FTP::FileInfo info;
                info.name = QString::fromStdString(e.name);
                info.size = static_cast<qint64>(e.size);
                info.isDirectory = e.isDirectory;
                // modifiedTime/permissions 保持默认(空)——见 common/ftptypes.h 里的说明。
                list.append(info);
            }
        }
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("目录加载失败"));
        QMetaObject::invokeMethod(
            this, [this, success, path, message, list] { emit directoryListed(success, path, message, list); },
            Qt::QueuedConnection);
    });
}

void Connection::mkdir(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.mkdir(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("创建目录失败"));
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::rmdir(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.rmdir(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("删除目录失败"));
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::deleteFile(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.removeFile(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("删除文件失败"));
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::rename(const QString& oldPath, const QString& newPath) {
    const std::string from = oldPath.toStdString();
    const std::string to = newPath.toStdString();
    pushJob([this, from, to](core::FtpClient& client) {
        const bool success = client.rename(from, to) == core::FtpResult::Ok;
        const QString message = success ? QString() : takeLastErrorOr(QStringLiteral("重命名失败"));
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}
