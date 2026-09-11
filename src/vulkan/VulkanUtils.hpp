#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vv::vulkan::utils {

constexpr std::array<const char*, 1> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};

struct SwapchainSupportDetails {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

// General helpers
std::string vkResultToString(VkResult r);
uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi);
uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice,
                             uint32_t typeFilter,
                             VkMemoryPropertyFlags props);
VkShaderModule createShaderModule(VkDevice device,
                                  const std::vector<char>& code,
                                  std::string& outError);

// Swapchain & device helpers
SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device,
                                              VkSurfaceKHR surface);
bool checkDeviceExtensionSupport(VkPhysicalDevice device);
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface,
                      uint32_t& outGraphicsFamily,
                      uint32_t& outPresentFamily);

VkSurfaceFormatKHR chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats);
VkPresentModeKHR choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes);
VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                            uint32_t width, uint32_t height);

uint32_t findGraphicsQueueFamily(VkPhysicalDevice device);
uint32_t findPresentQueueFamily(VkPhysicalDevice device,
                                VkSurfaceKHR surface);

} // namespace vv::vulkan::utils
