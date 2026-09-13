#include <QApplication>

#include "platform/CrashLog.hpp"
#include "ui/AppWindow.hpp"

int main(int argc, char** argv) {
	// Durable diagnostics (GUI-subsystem Release builds have no console:
	// see platform/CrashLog.hpp). Cleared per run; after a crash the file
	// holds the current run's trail + the exception record.
	vv::platform::crashLogClear();
	vv::platform::installCrashHandler();

	QApplication app(argc, argv);

	vv::ui::AppWindow window;
	window.showMaximized();

	return app.exec();
}
