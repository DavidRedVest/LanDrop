#pragma once

#include <QWidget>
#include <QModelIndex>
#include <QList>

class QTableView;
class QPushButton;
class TransferQueue;

// 传输队列可视化面板:进度条表格 + 暂停/继续/取消/删除/清除已完成 工具栏。
//
// "进度"列曾经用一个自绘的 QStyledItemDelegate(手动拼 QStyleOptionProgressBar
// 再调 style->drawControl())来画进度条,在 QMacStyle 上出过两次真实的渲染错位
// bug(进度条画到"方向"列底下、后面几行完全不画)——两次针对 QStyleOptionProgressBar
// 字段初始化方式的修复(见历史提交)都没能根治。现在换成 Qt 标准的
// QTableView::setIndexWidget() 方案:每一行"进度"格子里放一个真正的 QProgressBar
// 控件,由 Qt 自己管理这个控件的几何位置和重绘,不再需要我们手动猜
// QStyleOptionProgressBar 该怎么初始化才能在这个平台上画对。
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

    void onRowsInserted(const QModelIndex& parent, int first, int last);
    void onDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles);

private:
    void updateProgressCell(int row);

    TransferQueue* m_queue;
    QTableView* m_view;
};
