#include "vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include "core/CommandLine.hpp"

#include <optional>
#include <set>
#include <string>

namespace vv::vulkan::utils {

std::string vkResultToString(VkResult r) {
  switch (r) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_NOT_READY:
      return "VK_NOT_READY";
    case VK_TIMEOUT:
      return "VK_TIMEOUT";
    case VK_EVENT_SET:
      return "VK_EVENT_SET";
    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
      return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
      return "VkResult(" + std::to_string(static_cast<int>(r)) + ")";
  }
}

uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return std::max(lo, std::min(v, hi));
}

uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice,
                             uint32_t typeFilter,
                             VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeFilter & (1u << i)) != 0 &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  return UINT32_MAX;
}

VkShaderModule createShaderModule(VkDevice device,
                                  const std::vector<char>& code,
                                  std::string& outError) {
  if (code.empty() || (code.size() % 4) != 0) {
    outError = "Invalid SPIR-V shader binary (empty or not aligned).";
    return VK_NULL_HANDLE;
  }

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

  VkShaderModule module = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(device, &createInfo, nullptr, &module);
  if (r != VK_SUCCESS) {
    outError = "Failed to create shader module (" + vkResultToString(r) + ").";
    return VK_NULL_HANDLE;
  }
  return module;
}

SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device,
                                              VkSurfaceKHR surface) {
  SwapchainSupportDetails details{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface,
                                            &details.capabilities);

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
  details.formats.resize(formatCount);
  if (formatCount) {
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
                                         details.formats.data());
  }

  uint32_t presentModeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface,
                                            &presentModeCount, nullptr);
  details.presentModes.resize(presentModeCount);
  if (presentModeCount) {
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, surface, &presentModeCount, details.presentModes.data());
  }

  return details;
}

bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
  uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       nullptr);
  std::vector<VkExtensionProperties> available(extensionCount);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       available.data());

  std::set<std::string> required(kDeviceExtensions.begin(),
                                 kDeviceExtensions.end());
  for (const auto& ext : available) {
    required.erase(ext.extensionName);
  }
  return required.empty();
}

uint32_t findGraphicsQueueFamily(VkPhysicalDevice device) {
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           families.data());

  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

uint32_t findPresentQueueFamily(VkPhysicalDevice device,
                                VkSurfaceKHR surface) {
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    VkBool32 presentSupport = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
    if (presentSupport == VK_TRUE) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface,
                      uint32_t& outGraphicsFamily,
                      uint32_t& outPresentFamily) {
  const uint32_t graphicsFamily = findGraphicsQueueFamily(device);
  const uint32_t presentFamily = findPresentQueueFamily(device, surface);
  if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
    return false;
  }

  if (!checkDeviceExtensionSupport(device)) {
    return false;
  }

  const auto swapSupport = querySwapchainSupport(device, surface);
  if (swapSupport.formats.empty() || swapSupport.presentModes.empty()) {
    return false;
  }

  outGraphicsFamily = graphicsFamily;
  outPresentFamily = presentFamily;
  return true;
}

VkSurfaceFormatKHR chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {
  auto find = [&](VkFormat fmt) -> std::optional<VkSurfaceFormatKHR> {
    for (const auto& f : formats) {
      if (f.format == fmt &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        return f;
      }
    }
    return std::nullopt;
  };

  if (auto f = find(VK_FORMAT_R8G8B8A8_UNORM)) {
    return *f;
  }

  if (auto f = find(VK_FORMAT_B8G8R8A8_UNORM)) {
    return *f;
  }

  if (auto f = find(VK_FORMAT_B8G8R8A8_SRGB)) {
    return *f;
  }
  if (auto f = find(VK_FORMAT_R8G8B8A8_SRGB)) {
    return *f;
  }

  return formats[0];
}

VkPresentModeKHR choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) {
  const auto isSupported = [&modes](VkPresentModeKHR m) {
    return std::find(modes.begin(), modes.end(), m) != modes.end();
  };

  // --present immediate|mailbox|fifo overrides the preference (must still be
  // supported by the surface; otherwise ignored - a missing mode must not stop
  // the game). "fifo" restores vsync.
  const std::string requested = vv::core::options().present;
  if (!requested.empty()) {
    VkPresentModeKHR mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
    if (requested == "immediate") {
      mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (requested == "mailbox") {
      mode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (requested == "fifo") {
      mode = VK_PRESENT_MODE_FIFO_KHR;
    }
    if (mode != VK_PRESENT_MODE_MAX_ENUM_KHR && isSupported(mode)) {
      return mode;
    }
  }

  // Default: uncapped. IMMEDIATE shows the true frame rate (tearing is
  // possible), MAILBOX is vsync without latency but still caps at the
  // compositor rate, FIFO is the guaranteed fallback (vsync). Together
  // with the 0ms tick timer this removes the ~60 fps ceiling.
  if (isSupported(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
  }
  if (isSupported(VK_PRESENT_MODE_MAILBOX_KHR)) {
    return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                            uint32_t width, uint32_t height) {
  // A surface is allowed to have no size at all: a minimized window reports
  // currentExtent == (0, 0) on Win32 (the surface still reports an extent, so
  // the UINT32_MAX test below does not catch it). vkCreateSwapchainKHR rejects
  // a zero extent (VUID-VkSwapchainCreateInfoKHR-imageExtent-01689), so that
  // case falls through to the clamped requested size instead of being handed
  // to the driver.
  if (capabilities.currentExtent.width != UINT32_MAX &&
      capabilities.currentExtent.width > 0 &&
      capabilities.currentExtent.height > 0) {
    return capabilities.currentExtent;
  }

  // Clamped into what the surface supports, and never zero: a surface that
  // reports minImageExtent (0, 0) is not usable, but no driver may be handed
  // an extent of zero either.
  VkExtent2D actual{};
  actual.width = clampU32(width, std::max(1u, capabilities.minImageExtent.width),
                          capabilities.maxImageExtent.width);
  actual.height =
      clampU32(height, std::max(1u, capabilities.minImageExtent.height),
               capabilities.maxImageExtent.height);
  return actual;
}

} // namespace vv::vulkan::utils
