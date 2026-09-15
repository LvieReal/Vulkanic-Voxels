#pragma once

#include <cstdint>

namespace vv::platform {

// Identifies the windowing-system backend a native window belongs to.
// Mirrors the GLFW platform the application is running on
// (glfwGetPlatform) so the UI layer can describe its window in a
// renderer/platform-friendly way.
enum class NativeWindowKind : std::uint8_t {
	Unknown = 0,
	Win32,   // Microsoft Windows (HWND based)
	X11,     // Linux X11 (Display* / Window)
	Wayland, // Linux Wayland
	Cocoa,   // macOS
};

// Platform-agnostic bundle of native handles describing the window a
// renderer should draw into.
//
// It is filled in from GLFW (see platform/GlfwNativeWindow.hpp) and consumed
// by the Vulkan layer (see VulkanSurfaceFactory), so neither of those needs
// to know about the other's platform details.
struct NativeWindow final {
	NativeWindowKind kind = NativeWindowKind::Unknown;

	// Win32: HWND of the window.
	// Cocoa: NSView* to render into.
	// X11 / Wayland: unused (see the fields below).
	void* windowHandle = nullptr;

	// Win32: HINSTANCE of the module that owns the window (optional; the
	//        Vulkan surface factory falls back to GetModuleHandleW(nullptr)).
	// X11:     Display* of the connection that owns the window.
	// Wayland: wl_display* of the current connection.
	// Cocoa:   unused.
	void* displayHandle = nullptr;

	// X11 only: the X11 window id (Xlib's Window, an XID). Stored as 64-bit
	// so the struct layout does not depend on platform typedefs.
	std::uint64_t x11WindowId = 0;

	// Wayland only: wl_surface* backing the window.
	void* waylandSurface = nullptr;

	bool isValid() const {
		switch (kind) {
			case NativeWindowKind::Win32:
				return windowHandle != nullptr;
			case NativeWindowKind::X11:
				return displayHandle != nullptr && x11WindowId != 0;
			case NativeWindowKind::Wayland:
				return displayHandle != nullptr && waylandSurface != nullptr;
			case NativeWindowKind::Cocoa:
				return windowHandle != nullptr;
			case NativeWindowKind::Unknown:
				break;
		}
		return false;
	}
};

} // namespace vv::platform
