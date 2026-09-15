#pragma once

#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>

#include "platform/NativeWindow.hpp"

namespace vv::platform {

// Maps the GLFW platform the application runs on (glfwGetPlatform, GLFW 3.4+)
// onto the platform-agnostic NativeWindowKind.
NativeWindowKind nativeWindowKindFromGlfw(int glfwPlatform);

// Resolves the platform-agnostic NativeWindow description for a GLFWwindow.
//
// The window must belong to the current context. On failure the returned
// NativeWindow has kind Unknown and outError contains a human-readable
// reason.
//
// How the handles are obtained (GLFW's own native accessors):
//   Win32   glfwGetWin32Window (HWND); the HINSTANCE from the window's
//           GWLP_HINSTANCE (glfw has no module accessor).
//   X11     glfwGetX11Display (Display*) + glfwGetX11Window.
//   Wayland glfwGetWaylandDisplay (wl_display*) + glfwGetWaylandWindow.
//   macOS   glfwGetCocoaView (NSView*).
//
// A backend whose headers were not available at configure time (see
// VV_WINDOW_BACKEND_X11 / _WAYLAND in cmake/GameTarget.cmake) is compiled
// out, in which case resolution fails with the reason spelled out.
NativeWindow resolveNativeWindow(GLFWwindow* window, std::string& outError);

} // namespace vv::platform
