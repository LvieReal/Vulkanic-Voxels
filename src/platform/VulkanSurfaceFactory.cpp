#include "platform/VulkanSurfaceFactory.hpp"

// Platform WSI headers. Exactly one backend is active on Windows/macOS; on
// Linux the X11 (Xlib) and Wayland backends are compiled in when GLFW was
// built with them (VV_WINDOW_BACKEND_X11 / _WAYLAND, set from what GLFW can
// do - see cmake/GameTarget.cmake), and the right one is selected at run time
// from the NativeWindow kind reported by GLFW.
//
// Note: the Vulkan platform headers only declare the surface create-info
// structs; the windowing-system headers must be included first.
#ifndef VV_WINDOW_BACKEND_X11
#define VV_WINDOW_BACKEND_X11 0
#endif
#ifndef VV_WINDOW_BACKEND_WAYLAND
#define VV_WINDOW_BACKEND_WAYLAND 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#include <vulkan/vulkan_macos.h>
#else
#if VV_WINDOW_BACKEND_X11
#include <X11/Xlib.h>

#include <vulkan/vulkan_xlib.h>
#endif
#if VV_WINDOW_BACKEND_WAYLAND
#include <wayland-client-core.h>

#include <vulkan/vulkan_wayland.h>
#endif
#endif

namespace vv::platform {

namespace {

// WSI extension names for every backend. The Vulkan platform headers define
// the *_EXTENSION_NAME macros only for the platform they belong to, so
// literal strings (fixed by the Vulkan spec) are used for backends whose
// headers are not compiled in. This lets the extension list be built on every
// platform; a backend whose header is missing simply fails at surface
// creation with a clear message.
#if defined(_WIN32)
constexpr const char* const kWin32SurfaceExtension =
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
constexpr const char* const kWin32SurfaceExtension = "VK_KHR_win32_surface";
#endif

#if VV_WINDOW_BACKEND_X11
constexpr const char* const kX11SurfaceExtension =
		VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#else
constexpr const char* const kX11SurfaceExtension = "VK_KHR_xlib_surface";
#endif

#if VV_WINDOW_BACKEND_WAYLAND
constexpr const char* const kWaylandSurfaceExtension =
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
#else
constexpr const char* const kWaylandSurfaceExtension =
		"VK_KHR_wayland_surface";
#endif

#if defined(__APPLE__)
constexpr const char* const kMacOSSurfaceExtension =
		VK_MVK_MACOS_SURFACE_EXTENSION_NAME;
#else
constexpr const char* const kMacOSSurfaceExtension = "VK_MVK_macos_surface";
#endif

}  // namespace

std::vector<const char*> requiredVulkanInstanceExtensions(
		const NativeWindow& nativeWindow) {
	std::vector<const char*> extensions;
	extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

	switch (nativeWindow.kind) {
		case NativeWindowKind::Win32:
			extensions.push_back(kWin32SurfaceExtension);
			break;
		case NativeWindowKind::X11:
			extensions.push_back(kX11SurfaceExtension);
			break;
		case NativeWindowKind::Wayland:
			extensions.push_back(kWaylandSurfaceExtension);
			break;
		case NativeWindowKind::Cocoa:
			extensions.push_back(kMacOSSurfaceExtension);
			break;
		case NativeWindowKind::Unknown:
			break;
	}
	return extensions;
}

bool createVulkanSurface(VkInstance instance, const NativeWindow& nativeWindow,
												 VkSurfaceKHR* outSurface, std::string& outError) {
	if (instance == VK_NULL_HANDLE) {
		outError = "Cannot create Vulkan surface without a VkInstance.";
		return false;
	}
	if (outSurface == nullptr) {
		outError = "Cannot create Vulkan surface without an output handle.";
		return false;
	}
	*outSurface = VK_NULL_HANDLE;

#if defined(_WIN32)
	if (nativeWindow.kind != NativeWindowKind::Win32) {
		outError = "Only Win32 native windows are supported on Windows.";
		return false;
	}

	HINSTANCE hinstance = static_cast<HINSTANCE>(nativeWindow.displayHandle);
	if (hinstance == nullptr) {
		hinstance = GetModuleHandleW(nullptr);
	}

	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hinstance = hinstance;
	createInfo.hwnd = static_cast<HWND>(nativeWindow.windowHandle);

	// WSI entry points are resolved through the instance so the binary also
	// links against loader builds that do not export them directly.
	PFN_vkCreateWin32SurfaceKHR createSurface =
			reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
					vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
	if (createSurface == nullptr) {
		outError = "vkCreateWin32SurfaceKHR is not available in the Vulkan loader.";
		return false;
	}

	VkResult result = createSurface(instance, &createInfo, nullptr, outSurface);
	if (result != VK_SUCCESS) {
		outError = "Failed to create Win32 Vulkan surface (result " +
							 std::to_string(static_cast<int>(result)) + ").";
		return false;
	}
	return true;
#elif defined(__APPLE__)
	if (nativeWindow.kind != NativeWindowKind::Cocoa) {
		outError = "Only Cocoa native windows are supported on macOS.";
		return false;
	}

	// vkCreateMacOSSurfaceMVK is a MoltenVK extension function; resolve it
	// through the instance instead of assuming the loader exports it.
	PFN_vkCreateMacOSSurfaceMVK createSurface =
			reinterpret_cast<PFN_vkCreateMacOSSurfaceMVK>(
					vkGetInstanceProcAddr(instance, "vkCreateMacOSSurfaceMVK"));
	if (createSurface == nullptr) {
		outError =
				"vkCreateMacOSSurfaceMVK is not available. Rendering on macOS requires "
				"a Vulkan implementation with MoltenVK (VK_MVK_macos_surface).";
		return false;
	}

	VkMacOSSurfaceCreateInfoMVK createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
	createInfo.pView = nativeWindow.windowHandle;  // NSView*

	VkResult result = createSurface(instance, &createInfo, nullptr, outSurface);
	if (result != VK_SUCCESS) {
		outError = "Failed to create macOS (MoltenVK) Vulkan surface (result " +
							 std::to_string(static_cast<int>(result)) + ").";
		return false;
	}
	return true;
#else
	switch (nativeWindow.kind) {
#if VV_WINDOW_BACKEND_X11
		case NativeWindowKind::X11: {
			PFN_vkCreateXlibSurfaceKHR createSurface =
					reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
							vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
			if (createSurface == nullptr) {
				outError =
						"vkCreateXlibSurfaceKHR is not available in the Vulkan loader.";
				return false;
			}

			VkXlibSurfaceCreateInfoKHR createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
			createInfo.dpy = static_cast<Display*>(nativeWindow.displayHandle);
			createInfo.window = static_cast<Window>(nativeWindow.x11WindowId);

			VkResult result =
					createSurface(instance, &createInfo, nullptr, outSurface);
			if (result != VK_SUCCESS) {
				outError = "Failed to create X11 (Xlib) Vulkan surface (result " +
									 std::to_string(static_cast<int>(result)) + ").";
				return false;
			}
			return true;
		}
#else
		case NativeWindowKind::X11:
			outError =
					"The X11 Vulkan surface backend was not compiled in (GLFW was "
					"built without its X11 backend). Install the GLFW X11 "
					"dependencies (libxcb/libxrandr/libxinerama/libxcursor/libxi/"
					"libxkb, or libglfw3-dev with X11 support) and reconfigure.";
			return false;
#endif
#if VV_WINDOW_BACKEND_WAYLAND
		case NativeWindowKind::Wayland: {
			PFN_vkCreateWaylandSurfaceKHR createSurface =
					reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
							vkGetInstanceProcAddr(instance,
																		"vkCreateWaylandSurfaceKHR"));
			if (createSurface == nullptr) {
				outError =
						"vkCreateWaylandSurfaceKHR is not available in the Vulkan loader.";
				return false;
			}

			VkWaylandSurfaceCreateInfoKHR createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
			createInfo.display =
					static_cast<wl_display*>(nativeWindow.displayHandle);
			createInfo.surface =
					static_cast<wl_surface*>(nativeWindow.waylandSurface);

			VkResult result =
					createSurface(instance, &createInfo, nullptr, outSurface);
			if (result != VK_SUCCESS) {
				outError = "Failed to create Wayland Vulkan surface (result " +
									 std::to_string(static_cast<int>(result)) + ").";
				return false;
			}
			return true;
		}
#else
		case NativeWindowKind::Wayland:
			outError =
					"The Wayland Vulkan surface backend was not compiled in (GLFW "
					"was built without its Wayland backend). Install the Wayland "
					"client development headers (libwayland-dev + libxkbcommon-dev "
					"on Debian/Ubuntu) or run the game on X11.";
			return false;
#endif
		case NativeWindowKind::Win32:
		case NativeWindowKind::Cocoa:
			outError =
					"This native window kind is not supported on the current "
					"platform.";
			return false;
		case NativeWindowKind::Unknown:
			break;
	}

	outError = "Cannot create a Vulkan surface for an unknown native window.";
	return false;
#endif
}

}  // namespace vv::platform
