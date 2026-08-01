#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LanDrop");
    QCoreApplication::setApplicationName("LanDrop_Client");
    app.setWindowIcon(QIcon(":/icons/landrop.png"));

    MainWindow window;
    window.show();

    return app.exec();
}
