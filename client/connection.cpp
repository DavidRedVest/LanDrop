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

void Connection::threadMain() {
    core::FtpClient client;
    const core::FtpResult result = client.connect(m_host.toStdString(), m_port, 5000);
    if (result != core::FtpResult::Ok) {
        QMetaObject::invokeMethod(
            this, [this] { emit connectionError(QStringLiteral("连接失败,请检查地址、端口和网络")); },
            Qt::QueuedConnection);
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
        const QString message = success ? QString() : QStringLiteral("用户名或密码错误");
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
        const QString message = success ? QString() : QStringLiteral("目录加载失败");
        QMetaObject::invokeMethod(
            this, [this, success, path, message, list] { emit directoryListed(success, path, message, list); },
            Qt::QueuedConnection);
    });
}

void Connection::mkdir(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.mkdir(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : QStringLiteral("创建目录失败");
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::rmdir(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.rmdir(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : QStringLiteral("删除目录失败");
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::deleteFile(const QString& path) {
    const std::string p = path.toStdString();
    pushJob([this, p](core::FtpClient& client) {
        const bool success = client.removeFile(p) == core::FtpResult::Ok;
        const QString message = success ? QString() : QStringLiteral("删除文件失败");
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}

void Connection::rename(const QString& oldPath, const QString& newPath) {
    const std::string from = oldPath.toStdString();
    const std::string to = newPath.toStdString();
    pushJob([this, from, to](core::FtpClient& client) {
        const bool success = client.rename(from, to) == core::FtpResult::Ok;
        const QString message = success ? QString() : QStringLiteral("重命名失败");
        QMetaObject::invokeMethod(
            this, [this, success, message] { emit operationResult(success, message); }, Qt::QueuedConnection);
    });
}
