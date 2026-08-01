#pragma once

#include <QWidget>
#include <QStyledItemDelegate>

class QTableView;
class QPushButton;
class TransferQueue;

// 在"进度"列里把百分比画成真正的进度条,而不是纯文本。
class TransferProgressDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// 传输队列可视化面板:进度条表格 + 暂停/继续/取消/删除/清除已完成 工具栏。
class TransferWidget : public QWidget {
    Q_OBJECT

public:
    explicit TransferWidget(TransferQueue* queue, QWidget* parent = nullptr);

private slots:
    void onPause();
    void onResume();
    void onCancel();
    void onRemove();
    void onClearFinished();

private:
    TransferQueue* m_queue;
    QTableView* m_view;
};
