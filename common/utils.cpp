#include "utils.h"
#include <QRandomGenerator>

namespace FTP::Utils {

namespace {
constexpr int kSaltBytes = 16;
}

QString formatFileSize(qint64 size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double s = static_cast<double>(size);
    
    while (s >= 1024.0 && unitIndex < 4) {
        s /= 1024.0;
        unitIndex++;
    }
    
    return QString("%1 %2").arg(s, 0, 'f', unitIndex == 0 ? 0 : 2).arg(units[unitIndex]);
}

QString formatSpeed(qint64 bytesPerSecond) {
    return formatFileSize(bytesPerSecond) + "/s";
}

QString formatDuration(int seconds) {
    if (seconds < 60) {
        return QString("%1秒").arg(seconds);
    } else if (seconds < 3600) {
        return QString("%1分%2秒").arg(seconds / 60).arg(seconds % 60);
    } else {
        int h = seconds / 3600;
        int m = (seconds % 3600) / 60;
        return QString("%1时%2分").arg(h).arg(m);
    }
}

QByteArray fileHash(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(65536));
    }
    
    return hash.result();
}

bool isValidFileName(const QString& name) {
    if (name.isEmpty() || name.length() > 255) {
        return false;
    }
    
    // 检查非法字符
    static const QString illegalChars = "<>:\"/\\|?*";
    for (const QChar& c : illegalChars) {
        if (name.contains(c)) {
            return false;
        }
    }
    
    // 不允许以点开头（隐藏文件）或包含路径遍历
    if (name.startsWith('.') || name.contains("..") || name.contains("./") || name.contains(".\\")) {
        return false;
    }
    
    return true;
}

QString normalizePath(const QString& basePath, const QString& relativePath) {
    // relativePath 用协议里的虚拟根路径写法(以 "/" 开头,如 "/a/b.txt")。
    // QDir::absoluteFilePath() 一旦看到以 "/" 开头的参数会当成"已经是绝对路径"
    // 直接返回,完全绕过 basePath——必须先去掉前导 "/" 再拼接,否则下面的
    // "结果必须在 basePath 之内" 校验会因为路径完全对不上而每次都被钳制回
    // basePath 本身(而不是 basePath 下的目标文件),导致所有文件操作都读写
    // 到根目录上。
    QString rel = relativePath;
    while (rel.startsWith('/') || rel.startsWith('\\')) {
        rel.remove(0, 1);
    }

    QDir baseDir(basePath);
    QString cleanPath = QDir::cleanPath(baseDir.absoluteFilePath(rel));
    QString baseAbs = QDir(basePath).absolutePath();
    
    // 确保结果路径在basePath之内
    if (!cleanPath.startsWith(baseAbs)) {
        return baseAbs;
    }
    
    return cleanPath;
}

QString filePermissions(const QFileInfo& info) {
    QString perm;
    perm += info.isDir() ? 'd' : '-';
    perm += info.permission(QFile::ReadOwner) ? 'r' : '-';
    perm += info.permission(QFile::WriteOwner) ? 'w' : '-';
    perm += info.permission(QFile::ExeOwner) ? 'x' : '-';
    perm += info.permission(QFile::ReadGroup) ? 'r' : '-';
    perm += info.permission(QFile::WriteGroup) ? 'w' : '-';
    perm += info.permission(QFile::ExeGroup) ? 'x' : '-';
    perm += info.permission(QFile::ReadOther) ? 'r' : '-';
    perm += info.permission(QFile::WriteOther) ? 'w' : '-';
    perm += info.permission(QFile::ExeOther) ? 'x' : '-';
    return perm;
}

QString hashPassword(const QString& password) {
    QByteArray salt;
    salt.resize(kSaltBytes);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(salt.data()),
                                           kSaltBytes / sizeof(quint32));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(password.toUtf8());

    return QString("%1:%2").arg(QString(salt.toHex()), QString(hash.result().toHex()));
}

bool verifyPassword(const QString& password, const QString& storedHash) {
    const QStringList parts = storedHash.split(':');
    if (parts.size() != 2) {
        return false;
    }

    const QByteArray salt = QByteArray::fromHex(parts[0].toUtf8());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(password.toUtf8());

    return QString(hash.result().toHex()) == parts[1];
}

} // namespace FTP::Utils
