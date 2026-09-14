#include "platform/QtNativeWindowResolver.hpp"

#include <QByteArray>
#include <QGuiApplication>
#include <QString>
#include <QWidget>
#include <QWindow>

// Qt's private QtGui headers provide the QPA native interface, the only way
// to query some native handles (per-window Wayland wl_surface, XCB
// connection). Not every Qt distribution ships them, so the build system
// defines VV_HAVE_QT_QPA=1 only when they are available; otherwise public
// native interfaces cover Windows, macOS and X11.
#ifndef VV_HAVE_QT_QPA
#define VV_HAVE_QT_QPA 0
#endif

#if VV_HAVE_QT_QPA
#include <QtGui/qpa/qplatformnativeinterface.h>
#endif

// Public per-application native interfaces (Qt 6.2+, installed header). The
// interface structs only exist when Qt itself was built with the matching
// platform, which QT_CONFIG() reflects for the compiling application.
#include <QtGui/qguiapplication_platform.h>

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

#if VV_HAVE_QT_QPA
// Preferred path: the QPA native interface gives per-window resources.
void* nativeResource(QWindow* window, const char* resource) {
	QPlatformNativeInterface* native =
			QGuiApplication::platformNativeInterface();
	if (native == nullptr) {
		return nullptr;
	}
	return native->nativeResourceForWindow(QByteArray(resource), window);
}
#endif

}  // namespace

NativeWindow resolveNativeWindow(QWidget* widget, std::string& outError) {
	NativeWindow result;

	QWindow* window = nativeWindowFor(widget, outError);
	if (window == nullptr) {
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
		result.x11WindowId = static_cast<std::uint64_t>(widget->winId());
#if VV_HAVE_QT_QPA
		result.displayHandle = nativeResource(window, "connection");
#elif QT_CONFIG(xcb)
		// Fallback: public application-wide interface. xcb_connection_t* is
		// ABI-stable, so storing it needs no libxcb headers.
		if (auto* x11 =
						qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
			result.displayHandle = x11->connection();
		}
#else
		outError =
				"Qt was built without XCB support and without its private "
				"headers; cannot obtain the XCB connection.";
#endif
		if (!result.isValid() && outError.empty()) {
			outError = "Failed to obtain the XCB connection from Qt.";
		}
		return result;
	}

	if (platform == QLatin1String("wayland")) {
		result.kind = NativeWindowKind::Wayland;
#if VV_HAVE_QT_QPA
		result.displayHandle = nativeResource(window, "display");
		result.waylandSurface = nativeResource(window, "surface");
		if (!result.isValid()) {
			outError =
					"Failed to obtain the wl_display/wl_surface from Qt. Rendering "
					"into Qt widgets on Wayland requires a Wayland-capable Qt build.";
		}
#else
		// The public native interface only exposes the application-wide
		// wl_display, not the per-window wl_surface a Vulkan surface needs.
		outError =
				"Rendering into Qt widgets on Wayland requires Qt's private "
				"headers (qt6-base-private-dev on Debian/Ubuntu; other "
				"distributions ship them with the main Qt devel package or not "
				"at all). Alternatively run via XWayland: "
				"QT_QPA_PLATFORM=xcb ./game";
#endif
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
