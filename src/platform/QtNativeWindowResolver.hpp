#pragma once

#include <string>

#include "platform/NativeWindow.hpp"

class QWidget;

namespace vv::platform {

// Resolves the platform-agnostic NativeWindow description for a QWidget.
//
// The widget must already have a native window handle (call QWidget::winId()
// first, or rely on Qt::WA_NativeWindow). The returned kind depends on the QPA
// platform the application is running on ("windows", "xcb", "wayland",
// "cocoa"). On failure the returned NativeWindow has kind Unknown and
// outError contains a human-readable reason.
NativeWindow resolveNativeWindow(QWidget* widget, std::string& outError);

} // namespace vv::platform
