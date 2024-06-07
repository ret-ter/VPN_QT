#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ClientWindow window;
    window.show();
    return app.exec();
}
