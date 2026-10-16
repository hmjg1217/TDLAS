#include <QApplication>

#include "ui/main_window.h"

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("TDLAS"));
    application.setApplicationName(QStringLiteral("Hydrogen"));

    MainWindow window;
    window.show();

    return application.exec();
}
