#include "filemanager.h"
#include "../common/utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

using FTP::FileInfo;

FileManager::FileManager(const QString& rootPath)
    : m_rootPath(rootPath)
{
}

void FileManager::setRootPath(const QString& path) {
    m_rootPath = QDir(path).absolutePath();
}

QString FileManager::absolutePath(const QString& relativePath) const {
    return FTP::Utils::normalizePath(m_rootPath, relativePath);
}

bool FileManager::listDirectory(const QString& relativePath, QList<FileInfo>& outList, QString& errorMessage) const {
    const QString dirPath = absolutePath(relativePath);
    QDir dir(dirPath);
    if (!dir.exists()) {
        errorMessage = QStringLiteral("目录不存在: %1").arg(relativePath);
        return false;
    }

    outList.clear();
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& entry : entries) {
        FileInfo info;
        info.name = entry.fileName();
        info.size = entry.isDir() ? 0 : entry.size();
        info.isDirectory = entry.isDir();
        info.modifiedTime = entry.lastModified();
        info.permissions = FTP::Utils::filePermissions(entry);
        outList.append(info);
    }
    return true;
}

bool FileManager::mkdir(const QString& relativePath, QString& errorMessage) const {
    const QString target = absolutePath(relativePath);
    const QFileInfo targetInfo(target);
    if (!FTP::Utils::isValidFileName(targetInfo.fileName())) {
        errorMessage = QStringLiteral("非法目录名: %1").arg(targetInfo.fileName());
        return false;
    }
    QDir dir;
    if (!dir.mkpath(target)) {
        errorMessage = QStringLiteral("创建目录失败: %1").arg(relativePath);
        return false;
    }
    return true;
}

bool FileManager::rmdir(const QString& relativePath, QString& errorMessage) const {
    const QString target = absolutePath(relativePath);
    QDir dir(target);
    if (!dir.exists()) {
        errorMessage = QStringLiteral("目录不存在: %1").arg(relativePath);
        return false;
    }
    if (!dir.removeRecursively()) {
        errorMessage = QStringLiteral("删除目录失败: %1").arg(relativePath);
        return false;
    }
    return true;
}

bool FileManager::deleteFile(const QString& relativePath, QString& errorMessage) const {
    const QString target = absolutePath(relativePath);
    if (!QFile::exists(target)) {
        errorMessage = QStringLiteral("文件不存在: %1").arg(relativePath);
        return false;
    }
    if (!QFile::remove(target)) {
        errorMessage = QStringLiteral("删除文件失败: %1").arg(relativePath);
        return false;
    }
    return true;
}

bool FileManager::rename(const QString& oldRelativePath, const QString& newRelativePath, QString& errorMessage) const {
    const QString oldTarget = absolutePath(oldRelativePath);
    const QString newTarget = absolutePath(newRelativePath);
    const QFileInfo newInfo(newTarget);
    if (!FTP::Utils::isValidFileName(newInfo.fileName())) {
        errorMessage = QStringLiteral("非法名称: %1").arg(newInfo.fileName());
        return false;
    }
    if (!QFile::exists(oldTarget)) {
        errorMessage = QStringLiteral("源不存在: %1").arg(oldRelativePath);
        return false;
    }
    if (!QFile::rename(oldTarget, newTarget)) {
        errorMessage = QStringLiteral("重命名失败: %1").arg(oldRelativePath);
        return false;
    }
    return true;
}

bool FileManager::exists(const QString& relativePath) const {
    return QFileInfo::exists(absolutePath(relativePath));
}

qint64 FileManager::fileSize(const QString& relativePath) const {
    const QFileInfo info(absolutePath(relativePath));
    return info.exists() ? info.size() : 0;
}

std::unique_ptr<QFile> FileManager::openForRead(const QString& relativePath, qint64 offset, QString& errorMessage) const {
    auto file = std::make_unique<QFile>(absolutePath(relativePath));
    if (!file->open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法打开文件: %1").arg(relativePath);
        return nullptr;
    }
    if (offset > 0 && !file->seek(offset)) {
        errorMessage = QStringLiteral("定位文件偏移失败: %1").arg(relativePath);
        return nullptr;
    }
    return file;
}

std::unique_ptr<QFile> FileManager::openForWrite(const QString& relativePath, qint64 offset, QString& errorMessage) const {
    const QString target = absolutePath(relativePath);
    QDir().mkpath(QFileInfo(target).absolutePath());

    auto file = std::make_unique<QFile>(target);
    const QIODevice::OpenMode mode = (offset > 0) ? QIODevice::ReadWrite : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!file->open(mode)) {
        errorMessage = QStringLiteral("无法打开文件写入: %1").arg(relativePath);
        return nullptr;
    }
    if (offset > 0 && !file->seek(offset)) {
        errorMessage = QStringLiteral("定位文件偏移失败: %1").arg(relativePath);
        return nullptr;
    }
    return file;
}
