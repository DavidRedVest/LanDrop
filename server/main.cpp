#include "serverwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("LanDrop");
    QCoreApplication::setApplicationName("LanDrop_Server");
    app.setWindowIcon(QIcon(":/icons/landrop.png"));

    ServerWindow window;
    window.show();

    return app.exec();
}
