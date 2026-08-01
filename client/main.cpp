#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LanDrop");
    QCoreApplication::setApplicationName("LanDrop_Client");

    MainWindow window;
    window.show();

    return app.exec();
}
