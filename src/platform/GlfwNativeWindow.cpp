#include "platform/GlfwNativeWindow.hpp"

// The build passes -DGLFW_INCLUDE_NONE (see cmake/GameTarget.cmake); keeping
// the define here makes the intent visible next to the include.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

// glfw3native.h only declares the accessors whose platform is asked for. Ask
// for exactly the backends this build has (the same macros the surface
// factory uses), plus the native one on Windows/macOS.
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#if defined(VV_WINDOW_BACKEND_X11)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#if defined(VV_WINDOW_BACKEND_WAYLAND)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#endif

#include <GLFW/glfw3native.h>

#include <cstdio>

namespace vv::platform {

NativeWindowKind nativeWindowKindFromGlfw(int glfwPlatform) {
	switch (glfwPlatform) {
		case GLFW_PLATFORM_WIN32:
			return NativeWindowKind::Win32;
		case GLFW_PLATFORM_X11:
			return NativeWindowKind::X11;
		case GLFW_PLATFORM_WAYLAND:
			return NativeWindowKind::Wayland;
		case GLFW_PLATFORM_COCOA:
			return NativeWindowKind::Cocoa;
		default:
			return NativeWindowKind::Unknown;
	}
}

namespace {

const char* glfwPlatformName(int platform) {
	switch (platform) {
		case GLFW_PLATFORM_WIN32:
			return "win32";
		case GLFW_PLATFORM_X11:
			return "x11";
		case GLFW_PLATFORM_WAYLAND:
			return "wayland";
		case GLFW_PLATFORM_COCOA:
			return "cocoa";
		case GLFW_PLATFORM_NULL:
			return "null (no windowing system: GLFW was built without a backend, "
						 "or none was detected)";
		default:
			break;
	}
	return "unknown";
}

}  // namespace

NativeWindow resolveNativeWindow(GLFWwindow* window, std::string& outError) {
	NativeWindow result;
	if (window == nullptr) {
		outError = "Cannot resolve a native window for a null GLFW window.";
		return result;
	}

	const int platform = glfwGetPlatform();
	const NativeWindowKind kind = nativeWindowKindFromGlfw(platform);
	result.kind = kind;

	switch (kind) {
		case NativeWindowKind::Win32: {
#if defined(GLFW_EXPOSE_NATIVE_WIN32)
			HWND hwnd = glfwGetWin32Window(window);
			if (hwnd != nullptr) {
				result.windowHandle = hwnd;
				// HINSTANCE of the module that registered the window class.
				result.displayHandle = reinterpret_cast<void*>(
						GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
			}
#endif
			break;
		}
		case NativeWindowKind::X11: {
#if defined(GLFW_EXPOSE_NATIVE_X11)
			result.displayHandle = glfwGetX11Display();  // Display*
			result.x11WindowId =
					static_cast<std::uint64_t>(glfwGetX11Window(window));  // Window
#endif
			break;
		}
		case NativeWindowKind::Wayland: {
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
			result.displayHandle = glfwGetWaylandDisplay();  // wl_display*
			result.waylandSurface =
					glfwGetWaylandWindow(window);  // wl_surface*
#endif
			break;
		}
		case NativeWindowKind::Cocoa: {
#if defined(GLFW_EXPOSE_NATIVE_COCOA)
			result.windowHandle = glfwGetCocoaView(window);  // NSView*
#endif
			break;
		}
		case NativeWindowKind::Unknown:
			break;
	}

	if (result.isValid()) {
		return result;
	}

	if (kind == NativeWindowKind::Unknown) {
		outError = std::string("Unsupported GLFW platform '") +
							 glfwPlatformName(platform) +
							 "' for Vulkan rendering. Supported platforms: win32, "
							 "x11, wayland, cocoa.";
	} else {
		outError = std::string("Failed to obtain the native window handles from "
													 "GLFW on the ") +
							 glfwPlatformName(platform) + " backend";
		const char* description = nullptr;
		const int code = glfwGetError(&description);
		if (code != GLFW_NO_ERROR && description != nullptr) {
			outError += std::string(" (GLFW error ") + std::to_string(code) +
									": " + description + ")";
		} else {
			outError += " (this build has no accessor for it; see "
									"VV_WINDOW_BACKEND_X11/_WAYLAND in cmake/GameTarget.cmake)";
		}
		outError += ".";
	}

	result = NativeWindow{};
	result.kind = kind;
	return result;
}

}  // namespace vv::platform
