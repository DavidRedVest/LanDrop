#pragma once

#include <QString>
#include <QList>
#include <memory>

#include "../common/protocol.h"

class QFile;

// 把服务端根目录之外的一切文件系统访问都收敛到这里:所有相对路径先经过
// FTP::Utils::normalizePath 钳制在根目录之内,再落地成 QFile/QDir 操作。
class FileManager {
public:
    explicit FileManager(const QString& rootPath = QString());

    void setRootPath(const QString& path);
    QString rootPath() const { return m_rootPath; }

    // relativePath 为空或"/"表示根目录本身
    QString absolutePath(const QString& relativePath) const;

    bool listDirectory(const QString& relativePath, QList<FTP::FileInfo>& outList, QString& errorMessage) const;
    bool mkdir(const QString& relativePath, QString& errorMessage) const;
    bool rmdir(const QString& relativePath, QString& errorMessage) const;
    bool deleteFile(const QString& relativePath, QString& errorMessage) const;
    bool rename(const QString& oldRelativePath, const QString& newRelativePath, QString& errorMessage) const;

    bool exists(const QString& relativePath) const;
    // 文件不存在时返回 0,用于计算续传 offset
    qint64 fileSize(const QString& relativePath) const;

    // offset > 0 时从该偏移续写/续读;写入时如果 offset 为 0 会先截断文件
    std::unique_ptr<QFile> openForRead(const QString& relativePath, qint64 offset, QString& errorMessage) const;
    std::unique_ptr<QFile> openForWrite(const QString& relativePath, qint64 offset, QString& errorMessage) const;

private:
    QString m_rootPath;
};
