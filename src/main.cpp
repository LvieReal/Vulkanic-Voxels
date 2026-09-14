#include <QApplication>

#include "ui/AppWindow.hpp"

int main(int argc, char** argv) {
	QApplication app(argc, argv);

	vv::ui::AppWindow window;
	window.showMaximized();

	return app.exec();
}
