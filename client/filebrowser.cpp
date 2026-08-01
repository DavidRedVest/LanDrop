#include "filebrowser.h"
#include "connection.h"
#include "../common/utils.h"

#include <QLineEdit>
#include <QPushButton>
#include <QTreeView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

// ---- FileEntryModel ----

FileEntryModel::FileEntryModel(QObject* parent) : QAbstractTableModel(parent) {}

void FileEntryModel::setEntries(QList<Entry> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
}

int FileEntryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

int FileEntryModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 4;
}

QVariant FileEntryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size()) return {};
    const Entry& entry = m_entries.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return entry.isDir ? QStringLiteral("[%1]").arg(entry.name) : entry.name;
        case 1: return entry.isDir ? QString("--") : FTP::Utils::formatFileSize(entry.size);
        case 2: return entry.modified.isValid() ? entry.modified.toString("yyyy-MM-dd hh:mm") : QString();
        case 3: return entry.permissions;
        default: return {};
        }
    }
    return {};
}

QVariant FileEntryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return QStringLiteral("名称");
    case 1: return QStringLiteral("大小");
    case 2: return QStringLiteral("修改时间");
    case 3: return QStringLiteral("权限");
    default: return {};
    }
}

// ---- FileBrowserPanel ----

FileBrowserPanel::FileBrowserPanel(BrowserMode mode, QWidget* parent)
    : QWidget(parent)
    , m_mode(mode)
    , m_model(new FileEntryModel(this))
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto* toolbarLayout = new QHBoxLayout();
    m_upButton = new QPushButton(QStringLiteral("上级"), this);
    m_pathEdit = new QLineEdit(this);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    toolbarLayout->addWidget(m_upButton);
    toolbarLayout->addWidget(m_pathEdit, 1);
    toolbarLayout->addWidget(m_refreshButton);
    mainLayout->addLayout(toolbarLayout);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mainLayout->addWidget(m_view);

    connect(m_upButton, &QPushButton::clicked, this, &FileBrowserPanel::onUpClicked);
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FileBrowserPanel::onPathEditReturnPressed);
    connect(m_view, &QTreeView::doubleClicked, this, &FileBrowserPanel::onDoubleClicked);
    connect(m_view, &QTreeView::customContextMenuRequested, this, &FileBrowserPanel::onContextMenuRequested);

    if (m_mode == BrowserMode::Local) {
        navigateTo(QDir::homePath());
    } else {
        m_currentPath = "/";
        m_pathEdit->setText(m_currentPath);
    }
}

void FileBrowserPanel::setConnection(Connection* connection) {
    m_connection = connection;
    if (!m_connection) return;
    connect(m_connection, &Connection::directoryListed, this, &FileBrowserPanel::onRemoteDirectoryListed);
    connect(m_connection, &Connection::operationResult, this, &FileBrowserPanel::onRemoteOperationResult);
}

void FileBrowserPanel::refresh() {
    if (m_mode == BrowserMode::Local) {
        refreshLocal();
    } else {
        refreshRemote();
    }
}

void FileBrowserPanel::navigateTo(const QString& path) {
    m_currentPath = path;
    m_pathEdit->setText(path);
    refresh();
    emit pathChanged(path);
}

void FileBrowserPanel::refreshLocal() {
    QDir dir(m_currentPath);
    if (!dir.exists()) return;

    QList<FileEntryModel::Entry> entries;
    const QFileInfoList infos = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& info : infos) {
        FileEntryModel::Entry entry;
        entry.name = info.fileName();
        entry.size = info.isDir() ? 0 : info.size();
        entry.isDir = info.isDir();
        entry.modified = info.lastModified();
        entry.permissions = FTP::Utils::filePermissions(info);
        entries.append(entry);
    }
    m_model->setEntries(entries);
}

void FileBrowserPanel::refreshRemote() {
    if (!m_connection || !m_connection->isConnected()) return;
    m_connection->listDirectory(m_currentPath);
}

void FileBrowserPanel::onRemoteDirectoryListed(bool success, const QString& path, const QString& message,
                                                const QList<FTP::FileInfo>& list) {
    if (path != m_currentPath) return;
    if (!success) {
        emit statusMessage(QStringLiteral("目录加载失败: %1").arg(message));
        return;
    }
    QList<FileEntryModel::Entry> entries;
    for (const FTP::FileInfo& info : list) {
        FileEntryModel::Entry entry;
        entry.name = info.name;
        entry.size = info.size;
        entry.isDir = info.isDirectory;
        entry.modified = info.modifiedTime;
        entry.permissions = info.permissions;
        entries.append(entry);
    }
    m_model->setEntries(entries);
}

void FileBrowserPanel::onRemoteOperationResult(bool success, const QString& message) {
    if (success) {
        refresh();
    } else {
        QMessageBox::warning(this, QStringLiteral("操作失败"), message);
    }
}

QString FileBrowserPanel::joinPath(const QString& base, const QString& name) const {
    if (m_mode == BrowserMode::Local) {
        return QDir(base).absoluteFilePath(name);
    }
    return base == "/" ? "/" + name : base + "/" + name;
}

QString FileBrowserPanel::parentPath(const QString& path) const {
    if (m_mode == BrowserMode::Local) {
        QDir dir(path);
        if (!dir.cdUp()) return path;
        return dir.absolutePath();
    }
    if (path == "/" || path.isEmpty()) return "/";
    QString trimmed = path;
    if (trimmed.endsWith('/')) trimmed.chop(1);
    const int lastSlash = trimmed.lastIndexOf('/');
    return lastSlash <= 0 ? "/" : trimmed.left(lastSlash);
}

void FileBrowserPanel::onDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    const FileEntryModel::Entry& entry = m_model->entryAt(index.row());
    if (entry.isDir) {
        navigateTo(joinPath(m_currentPath, entry.name));
    }
}

void FileBrowserPanel::onUpClicked() {
    navigateTo(parentPath(m_currentPath));
}

void FileBrowserPanel::onPathEditReturnPressed() {
    navigateTo(m_pathEdit->text());
}

bool FileBrowserPanel::hasSelection() const {
    return m_view->selectionModel() && !m_view->selectionModel()->selectedRows().isEmpty();
}

void FileBrowserPanel::onContextMenuRequested(const QPoint& pos) {
    const QModelIndex index = m_view->indexAt(pos);
    const bool onEntry = index.isValid();

    QMenu menu(this);
    QAction* refreshAction = menu.addAction(QStringLiteral("刷新"));
    QAction* mkdirAction = menu.addAction(QStringLiteral("新建文件夹"));
    QAction* deleteAction = onEntry ? menu.addAction(QStringLiteral("删除")) : nullptr;
    QAction* renameAction = onEntry ? menu.addAction(QStringLiteral("重命名")) : nullptr;
    menu.addSeparator();
    QAction* transferAction = nullptr;
    if (m_mode == BrowserMode::Local) {
        transferAction = menu.addAction(QStringLiteral("上传到远程当前目录"));
        transferAction->setEnabled(hasSelection());
    } else {
        transferAction = menu.addAction(QStringLiteral("下载到本地当前目录"));
        transferAction->setEnabled(hasSelection());
    }

    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == refreshAction) {
        refresh();
    } else if (chosen == mkdirAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"), QStringLiteral("文件夹名称:"),
                                                     QLineEdit::Normal, QString(), &ok);
        if (!ok || name.isEmpty()) return;
        if (m_mode == BrowserMode::Local) {
            QDir(m_currentPath).mkdir(name);
            refresh();
        } else if (m_connection) {
            m_connection->mkdir(joinPath(m_currentPath, name));
        }
    } else if (chosen == deleteAction) {
        const FileEntryModel::Entry& entry = m_model->entryAt(index.row());
        const auto reply = QMessageBox::question(this, QStringLiteral("删除"),
                                                   QStringLiteral("确定删除 %1 ?").arg(entry.name));
        if (reply != QMessageBox::Yes) return;
        const QString fullPath = joinPath(m_currentPath, entry.name);
        if (m_mode == BrowserMode::Local) {
            if (entry.isDir) QDir(fullPath).removeRecursively();
            else QFile::remove(fullPath);
            refresh();
        } else if (m_connection) {
            if (entry.isDir) m_connection->rmdir(fullPath);
            else m_connection->deleteFile(fullPath);
        }
    } else if (chosen == renameAction) {
        const FileEntryModel::Entry& entry = m_model->entryAt(index.row());
        bool ok = false;
        const QString newName = QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("新名称:"),
                                                        QLineEdit::Normal, entry.name, &ok);
        if (!ok || newName.isEmpty() || newName == entry.name) return;
        const QString oldPath = joinPath(m_currentPath, entry.name);
        const QString newPath = joinPath(m_currentPath, newName);
        if (m_mode == BrowserMode::Local) {
            QFile::rename(oldPath, newPath);
            refresh();
        } else if (m_connection) {
            m_connection->rename(oldPath, newPath);
        }
    } else if (chosen == transferAction) {
        const QModelIndexList rows = m_view->selectionModel()->selectedRows(0);
        if (m_mode == BrowserMode::Local) {
            QStringList paths;
            for (const QModelIndex& rowIndex : rows) {
                const FileEntryModel::Entry& entry = m_model->entryAt(rowIndex.row());
                if (!entry.isDir) paths.append(joinPath(m_currentPath, entry.name));
            }
            if (!paths.isEmpty()) emit uploadRequested(paths);
        } else {
            QList<QPair<QString, qint64>> files;
            for (const QModelIndex& rowIndex : rows) {
                const FileEntryModel::Entry& entry = m_model->entryAt(rowIndex.row());
                if (!entry.isDir) files.append({joinPath(m_currentPath, entry.name), entry.size});
            }
            if (!files.isEmpty()) emit downloadRequested(files);
        }
    }
}
