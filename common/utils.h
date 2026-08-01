#pragma once

#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>

namespace FTP::Utils {

// 格式化文件大小为人类可读格式
QString formatFileSize(qint64 size);

// 格式化传输速度
QString formatSpeed(qint64 bytesPerSecond);

// 格式化时间（秒）
QString formatDuration(int seconds);

// 计算文件哈希（用于校验）
QByteArray fileHash(const QString& filePath);

// 安全的文件名验证
bool isValidFileName(const QString& name);

// 规范化路径（防止目录遍历攻击）
QString normalizePath(const QString& basePath, const QString& relativePath);

// 获取文件权限字符串
QString filePermissions(const QFileInfo& info);

// 生成"salt:hash"形式的密码哈希(SHA-256,随机 salt),用于服务端用户表存储,
// 绝不应保存明文密码
QString hashPassword(const QString& password);

// 校验明文密码是否匹配 hashPassword() 生成的哈希
bool verifyPassword(const QString& password, const QString& storedHash);

} // namespace FTP::Utils
