#pragma once

#include <QWidget>
#include <QAbstractTableModel>
#include <QString>
#include <QList>
#include <QDateTime>

#include "../common/protocol.h"

class QLineEdit;
class QPushButton;
class QTreeView;
class Connection;

enum class BrowserMode { Local, Remote };

// Local/Remote 两种模式共用的"当前目录平铺列表"模型:Name/Size/Modified/Permissions。
class FileEntryModel : public QAbstractTableModel {
    Q_OBJECT

public:
    struct Entry {
        QString name;
        qint64 size = 0;
        bool isDir = false;
        QDateTime modified;
        QString permissions;
    };

    explicit FileEntryModel(QObject* parent = nullptr);

    void setEntries(QList<Entry> entries);
    const Entry& entryAt(int row) const { return m_entries.at(row); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QList<Entry> m_entries;
};

// 可复用的目录浏览面板:Local 模式浏览本机文件系统,Remote 模式通过 Connection
// 浏览服务端当前目录。两者使用同一套 UI 和 FileEntryModel,只是数据来源不同。
class FileBrowserPanel : public QWidget {
    Q_OBJECT

public:
    explicit FileBrowserPanel(BrowserMode mode, QWidget* parent = nullptr);

    void setConnection(Connection* connection); // 仅 Remote 模式需要
    QString currentPath() const { return m_currentPath; }
    void refresh();
    void navigateTo(const QString& path);

    bool hasSelection() const;

signals:
    void pathChanged(const QString& path);
    void uploadRequested(const QStringList& localAbsolutePaths); // 仅 Local 面板发出
    // 仅 Remote 面板发出;每项为 {远程完整路径, 文件大小},下载进度条需要用到大小
    void downloadRequested(const QList<QPair<QString, qint64>>& remoteFiles);
    void statusMessage(const QString& message);

private slots:
    void onDoubleClicked(const QModelIndex& index);
    void onUpClicked();
    void onPathEditReturnPressed();
    void onContextMenuRequested(const QPoint& pos);

    void onRemoteDirectoryListed(bool success, const QString& path, const QString& message,
                                  const QList<FTP::FileInfo>& list);
    void onRemoteOperationResult(bool success, const QString& message);

private:
    void refreshLocal();
    void refreshRemote();
    QString joinPath(const QString& base, const QString& name) const;
    QString parentPath(const QString& path) const;

    BrowserMode m_mode;
    Connection* m_connection = nullptr;
    QString m_currentPath;

    QLineEdit* m_pathEdit;
    QPushButton* m_upButton;
    QPushButton* m_refreshButton;
    QTreeView* m_view;
    FileEntryModel* m_model;
};
