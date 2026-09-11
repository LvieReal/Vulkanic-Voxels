#include "platform/QtNativeWindowResolver.hpp"

#include <QByteArray>
#include <QGuiApplication>
#include <QString>
#include <QWidget>
#include <QWindow>
#include <QtGui/qpa/qplatformnativeinterface.h>

namespace vv::platform {

namespace {

QWindow* nativeWindowFor(QWidget* widget, std::string& outError) {
	if (widget == nullptr) {
		outError = "Cannot resolve a native window for a null widget.";
		return nullptr;
	}

	// Make sure the widget is backed by a real native window.
	(void)widget->winId();

	QWindow* window = widget->windowHandle();
	if (window == nullptr || window->handle() == nullptr) {
		outError =
				"Widget has no native platform window yet (this should not happen "
				"for Qt::WA_NativeWindow widgets).";
		return nullptr;
	}
	return window;
}

}  // namespace

NativeWindow resolveNativeWindow(QWidget* widget, std::string& outError) {
	NativeWindow result;

	QWindow* window = nativeWindowFor(widget, outError);
	if (window == nullptr) {
		return result;
	}

	QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
	if (native == nullptr) {
		outError = "Qt did not provide a platform native interface.";
		return result;
	}

	const QString platform = QGuiApplication::platformName();

	if (platform == QLatin1String("windows")) {
		result.kind = NativeWindowKind::Win32;
		result.windowHandle = reinterpret_cast<void*>(widget->winId());  // HWND
		return result;
	}

	if (platform == QLatin1String("xcb")) {
		result.kind = NativeWindowKind::Xcb;
		result.displayHandle =
				native->nativeResourceForWindow(QByteArrayLiteral("connection"), window);
		result.x11WindowId = static_cast<std::uint64_t>(widget->winId());
		if (!result.isValid()) {
			outError = "Failed to obtain the XCB connection from Qt.";
		}
		return result;
	}

	if (platform == QLatin1String("wayland")) {
		result.kind = NativeWindowKind::Wayland;
		result.displayHandle =
				native->nativeResourceForWindow(QByteArrayLiteral("display"), window);
		result.waylandSurface =
				native->nativeResourceForWindow(QByteArrayLiteral("surface"), window);
		if (!result.isValid()) {
			outError =
					"Failed to obtain the wl_display/wl_surface from Qt. Rendering into "
					"Qt widgets on Wayland requires a Wayland-capable Qt build.";
		}
		return result;
	}

	if (platform == QLatin1String("cocoa")) {
		result.kind = NativeWindowKind::Cocoa;
		// On Cocoa, winId() returns the NSView pointer.
		result.windowHandle = reinterpret_cast<void*>(widget->winId());
		return result;
	}

	outError = "Unsupported Qt platform '" + platform.toStdString() +
						 "' for Vulkan rendering. Supported platforms: windows, xcb, "
						 "wayland, cocoa.";
	return result;
}

}  // namespace vv::platform
