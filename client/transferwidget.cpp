#include "transferwidget.h"
#include "transfer.h"

#include <QTableView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QStyleOptionProgressBar>
#include <QApplication>
#include <QWidget>
#include <algorithm>
#include <functional>

void TransferProgressDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    const qint64 total = index.data(TransferQueue::TotalSizeRole).toLongLong();
    const qint64 transferred = index.data(TransferQueue::BytesTransferredRole).toLongLong();

    if (total <= 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const int percent = static_cast<int>(qBound<qint64>(qint64(0), transferred * 100 / total, qint64(100)));

    // save/restore around the style call: QTableView reuses one QPainter across every
    // cell in a paint pass, so any clip/pen/transform this delegate leaves dirty would
    // otherwise bleed into whichever cell paints next.
    painter->save();
    QStyleOptionProgressBar barOption;
    // initFrom() seeds state/palette/direction/fontMetrics from the actual viewport widget.
    // Without it, QStyleOptionProgressBar::state defaults to State_None (not Enabled, not
    // Active, not Horizontal) — on QMacStyle specifically this incomplete state produces
    // wrong/stale geometry (observed: first row's bar painted under the wrong column
    // entirely, later rows painted nothing at all), not just a visual glitch. rect is
    // overwritten afterward with this cell's own rect since initFrom() sets it to the
    // whole widget's rect.
    if (option.widget) {
        barOption.initFrom(option.widget);
    }
    barOption.rect = option.rect.adjusted(2, 2, -2, -2);
    barOption.state |= QStyle::State_Enabled | QStyle::State_Active | QStyle::State_Horizontal;
    barOption.direction = option.direction;
    barOption.fontMetrics = option.fontMetrics;
    barOption.palette = option.palette;
    barOption.minimum = 0;
    barOption.maximum = 100;
    barOption.progress = percent;
    barOption.text = QStringLiteral("%1%").arg(percent);
    barOption.textVisible = true;
    barOption.textAlignment = Qt::AlignCenter;

    QStyle* style = option.widget ? option.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ProgressBar, &barOption, painter, option.widget);
    painter->restore();
}

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
    m_view->setItemDelegateForColumn(TransferQueue::ColProgress, new TransferProgressDelegate(m_view));
    mainLayout->addWidget(m_view);

    connect(pauseButton, &QPushButton::clicked, this, &TransferWidget::onPause);
    connect(resumeButton, &QPushButton::clicked, this, &TransferWidget::onResume);
    connect(cancelButton, &QPushButton::clicked, this, &TransferWidget::onCancel);
    connect(removeButton, &QPushButton::clicked, this, &TransferWidget::onRemove);
    connect(clearButton, &QPushButton::clicked, this, &TransferWidget::onClearFinished);
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
