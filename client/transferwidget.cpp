#include "transferwidget.h"
#include "transfer.h"

#include <QTableView>
#include <QPushButton>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <algorithm>
#include <functional>

TransferWidget::TransferWidget(TransferQueue* queue, QWidget* parent)
    : QWidget(parent)
    , m_queue(queue)
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto* toolbarLayout = new QHBoxLayout();
    auto* pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    auto* resumeButton = new QPushButton(QStringLiteral("继续"), this);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    auto* removeButton = new QPushButton(QStringLiteral("删除"), this);
    auto* clearButton = new QPushButton(QStringLiteral("清除已完成"), this);
    toolbarLayout->addWidget(pauseButton);
    toolbarLayout->addWidget(resumeButton);
    toolbarLayout->addWidget(cancelButton);
    toolbarLayout->addWidget(removeButton);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(clearButton);
    mainLayout->addLayout(toolbarLayout);

    m_view = new QTableView(this);
    m_view->setModel(m_queue);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->horizontalHeader()->setSectionResizeMode(TransferQueue::ColName, QHeaderView::Stretch);
    mainLayout->addWidget(m_view);

    connect(pauseButton, &QPushButton::clicked, this, &TransferWidget::onPause);
    connect(resumeButton, &QPushButton::clicked, this, &TransferWidget::onResume);
    connect(cancelButton, &QPushButton::clicked, this, &TransferWidget::onCancel);
    connect(removeButton, &QPushButton::clicked, this, &TransferWidget::onRemove);
    connect(clearButton, &QPushButton::clicked, this, &TransferWidget::onClearFinished);

    // 进度条:每行"进度"格子里放一个真正的 QProgressBar(见头文件注释,替换掉
    // 之前两次都没修好的自绘 delegate)。rowsInserted 时创建;之后只在 dataChanged
    // 命中这一行时更新数值——不需要在这里处理"行被删除"的清理,QAbstractItemView
    // 对通过 setIndexWidget() 挂上去的控件本来就是按 persistent index 跟踪的,
    // 所在行被移除时 Qt 自己会删除对应的控件。
    connect(m_queue, &QAbstractItemModel::rowsInserted, this, &TransferWidget::onRowsInserted);
    connect(m_queue, &QAbstractItemModel::dataChanged, this, &TransferWidget::onDataChanged);
    for (int row = 0; row < m_queue->rowCount(); ++row) onRowsInserted(QModelIndex(), row, row);
}

void TransferWidget::onPause() {
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) m_queue->pauseRow(index.row());
}

void TransferWidget::onResume() {
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) m_queue->resumeRow(index.row());
}

void TransferWidget::onCancel() {
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) m_queue->cancelRow(index.row());
}

void TransferWidget::onRemove() {
    QList<int> rows;
    for (const QModelIndex& index : m_view->selectionModel()->selectedRows()) rows.append(index.row());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) m_queue->removeRow(row);
}

void TransferWidget::onClearFinished() {
    m_queue->clearFinished();
}

void TransferWidget::onRowsInserted(const QModelIndex& parent, int first, int last) {
    if (parent.isValid()) return; // 这个表格模型没有嵌套层级
    for (int row = first; row <= last; ++row) {
        auto* bar = new QProgressBar(m_view);
        bar->setRange(0, 100);
        bar->setTextVisible(true);
        bar->setAlignment(Qt::AlignCenter);
        m_view->setIndexWidget(m_queue->index(row, TransferQueue::ColProgress), bar);
        updateProgressCell(row);
    }
}

void TransferWidget::onDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight,
                                    const QList<int>& /*roles*/) {
    if (topLeft.parent().isValid()) return;
    for (int row = topLeft.row(); row <= bottomRight.row(); ++row) updateProgressCell(row);
}

void TransferWidget::updateProgressCell(int row) {
    const QModelIndex progressIndex = m_queue->index(row, TransferQueue::ColProgress);
    auto* bar = qobject_cast<QProgressBar*>(m_view->indexWidget(progressIndex));
    if (!bar) return;

    const qint64 total = progressIndex.data(TransferQueue::TotalSizeRole).toLongLong();
    if (total <= 0) {
        // STOR 上传方向服务端不预先知道总大小、任务还没连接上等情况:总量未知,
        // 没法算百分比,不显示进度条(和之前 ColProgress 文本 "--" 的语义一致)。
        bar->setVisible(false);
        return;
    }
    bar->setVisible(true);
    const qint64 transferred = progressIndex.data(TransferQueue::BytesTransferredRole).toLongLong();
    const int percent = static_cast<int>(qBound<qint64>(qint64(0), transferred * 100 / total, qint64(100)));
    bar->setValue(percent);
    bar->setFormat(QStringLiteral("%1%").arg(percent));
}
