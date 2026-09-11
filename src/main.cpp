#include <QApplication>

#include "ui/AppWindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    vv::ui::AppWindow window;
    window.showMaximized();

    return app.exec();
}
