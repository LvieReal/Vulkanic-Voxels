#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "platform/NativeWindow.hpp"

namespace vv::platform {

// Returns the Vulkan instance extensions that must be enabled for
// createVulkanSurface() to work with the given native window. The first entry
// is always VK_KHR_surface, followed by the WSI extension matching the native
// window kind (e.g. VK_KHR_win32_surface or VK_KHR_xcb_surface).
std::vector<const char*> requiredVulkanInstanceExtensions(
		const NativeWindow& nativeWindow);

// Creates a VkSurfaceKHR for the native window.
//
// The surface is owned by the caller: it must be destroyed with
// vkDestroySurfaceKHR() before the VkInstance is destroyed.
//
// Returns false and fills outError with a human-readable message when the
// platform backend is unavailable or surface creation failed.
bool createVulkanSurface(VkInstance instance, const NativeWindow& nativeWindow,
												 VkSurfaceKHR* outSurface, std::string& outError);

} // namespace vv::platform
