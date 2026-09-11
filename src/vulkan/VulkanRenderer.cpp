#include "vulkan/VulkanRenderer.h"

#include "core/RuntimePaths.h"
#include "core/ShaderLoader.h"
#include "voxel/World.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

namespace vv::vulkan {

namespace {

constexpr std::array<const char *, 1> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};

struct PushConstants {
  glm::uvec4 screen{};    // x=width, y=height, z=bgra, w=frame
  glm::vec4 camera{};     // x=tanHalfFov
  glm::uvec4 chunkSize{}; // xyz=chunk voxel dims
  glm::vec4 voxelSize{};  // xyz=voxel size in world units
};

struct SceneUBO {
  glm::vec4 camPos{};
  glm::vec4 camForward{};
  glm::vec4 camRight{};
  glm::vec4 camUp{};
  glm::vec4 lightDir{};
  glm::vec4 lightColor{};
  glm::vec4 skyLow{};
  glm::vec4 skyHigh{};
  glm::vec4 misc{}; // x = timeSeconds
};

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
                             uint32_t typeFilter, VkMemoryPropertyFlags props) {
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

} // namespace

VulkanRenderer::~VulkanRenderer() { cleanup(); }

bool VulkanRenderer::init(const InitInfo &info, std::string &outError) {
  if (m_initialized) {
    return true;
  }
  if (!info.hwnd) {
    outError = "Invalid HWND.";
    return false;
  }

  if (!createInstance(outError) || !createSurface(info, outError) ||
      !pickPhysicalDevice(outError) || !createDevice(outError) ||
      !createDescriptorSetLayout(outError) || !createCommandPool(outError) ||
      !createVoxelWorldAndUpload(outError) || !createSceneResources(outError)) {
    cleanup();
    return false;
  }

  const uint32_t width = std::max(1u, info.width);
  const uint32_t height = std::max(1u, info.height);
  if (!createSwapchain(width, height, outError) ||
      !createCommandBuffers(outError) || !createSyncObjects(outError)) {
    cleanup();
    return false;
  }

  m_initialized = true;
  return true;
}

void VulkanRenderer::resize(uint32_t width, uint32_t height) {
  if (!m_initialized || width == 0 || height == 0) {
    return;
  }

  m_framebufferResized = true;
  std::string error;
  (void)recreateSwapchain(width, height, error);
}

void VulkanRenderer::drawFrame() {
  if (!m_initialized) {
    return;
  }

  vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE,
                  UINT64_MAX);

  if (m_sceneBufferMapped) {
    SceneUBO ubo{};
    const glm::vec3 f = m_camera.forward();
    const glm::vec3 r = m_camera.right();
    const glm::vec3 u = m_camera.up();

    ubo.camPos = glm::vec4(m_camera.position(), 1.0f);
    ubo.camForward = glm::vec4(f, 0.0f);
    ubo.camRight = glm::vec4(r, 0.0f);
    ubo.camUp = glm::vec4(u, 0.0f);

    ubo.lightDir = glm::vec4(glm::normalize(m_lightDir), 0.0f);
    ubo.lightColor = glm::vec4(m_lightColor, 0.0f);
    ubo.skyLow = glm::vec4(m_skyLow, 0.0f);
    ubo.skyHigh = glm::vec4(m_skyHigh, 0.0f);
    ubo.misc = glm::vec4(m_timeSeconds, 0.0f, 0.0f, 0.0f);

    std::memcpy(m_sceneBufferMapped, &ubo, sizeof(ubo));
  }

  uint32_t imageIndex = 0;
  VkResult acquire = vkAcquireNextImageKHR(
      m_device, m_swapchain, UINT64_MAX,
      m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);

  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    std::string error;
    (void)recreateSwapchain(m_swapchainExtent.width, m_swapchainExtent.height,
                            error);
    return;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    return;
  }

  vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

  vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
  std::string recordError;
  if (!recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex,
                           recordError)) {
    return;
  }

  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrame];
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentFrame];

  if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo,
                    m_inFlightFences[m_currentFrame]) != VK_SUCCESS) {
    return;
  }

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[m_currentFrame];
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &m_swapchain;
  presentInfo.pImageIndices = &imageIndex;

  VkResult present = vkQueuePresentKHR(m_presentQueue, &presentInfo);
  if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR ||
      m_framebufferResized) {
    m_framebufferResized = false;
    std::string error;
    (void)recreateSwapchain(m_swapchainExtent.width, m_swapchainExtent.height,
                            error);
  }

  m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
  ++m_frameCounter;
}

void VulkanRenderer::cleanup() {
  if (m_device) {
    vkDeviceWaitIdle(m_device);
  }

  cleanupSwapchain();
  cleanupSceneResources();
  cleanupVoxelResources();

  if (m_descriptorSetLayout) {
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    m_descriptorSetLayout = VK_NULL_HANDLE;
  }
  if (m_pipelineLayout) {
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
  }

  for (size_t i = 0; i < m_imageAvailableSemaphores.size(); ++i) {
    if (m_imageAvailableSemaphores[i]) {
      vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
    }
    if (m_renderFinishedSemaphores[i]) {
      vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
    }
    if (m_inFlightFences[i]) {
      vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
    }
  }
  m_imageAvailableSemaphores.clear();
  m_renderFinishedSemaphores.clear();
  m_inFlightFences.clear();

  if (m_commandPool) {
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
  }

  if (m_device) {
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
  }

  if (m_surface) {
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
  }

  if (m_instance) {
    vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
  }

  m_initialized = false;
}

void VulkanRenderer::setCamera(const vv::core::Camera &camera,
                               float timeSeconds) {
  m_camera = camera;
  m_timeSeconds = timeSeconds;
}

void VulkanRenderer::setWorldConfig(const glm::uvec3 &chunkSizeVoxels,
                                    const glm::vec3 &voxelSize) {
  if (m_initialized) {
    return;
  }

  m_chunkSizeVoxels = glm::max(chunkSizeVoxels, glm::uvec3(1u, 1u, 1u));
  m_voxelSize = glm::max(voxelSize, glm::vec3(1e-3f, 1e-3f, 1e-3f));
}

bool VulkanRenderer::createInstance(std::string &outError) {
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  auto *fpEnumerateInstanceVersion =
      reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
          vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
  if (fpEnumerateInstanceVersion) {
    fpEnumerateInstanceVersion(&loaderVersion);
  }

  const uint32_t requestedApiVersion =
      std::min(loaderVersion, VK_API_VERSION_1_4);

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "Vulkanic Voxels";
  appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  appInfo.pEngineName = "VulkanicVoxels";
  appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  appInfo.apiVersion = requestedApiVersion;

  std::vector<const char *> extensions = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
  };

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  VkResult r = vkCreateInstance(&createInfo, nullptr, &m_instance);
  if (r != VK_SUCCESS) {
    outError = "Failed to create Vulkan instance (" + vkResultToString(r) +
               "). Vulkan might be missing or unsupported on this system.";
    return false;
  }
  return true;
}

bool VulkanRenderer::createSurface(const InitInfo &info,
                                   std::string &outError) {
  VkWin32SurfaceCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  createInfo.hinstance = info.hinstance;
  createInfo.hwnd = info.hwnd;

  VkResult r =
      vkCreateWin32SurfaceKHR(m_instance, &createInfo, nullptr, &m_surface);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create Win32 Vulkan surface (" + vkResultToString(r) + ").";
    return false;
  }
  return true;
}

uint32_t VulkanRenderer::findGraphicsQueueFamily(VkPhysicalDevice device) {
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

uint32_t VulkanRenderer::findPresentQueueFamily(VkPhysicalDevice device) {
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    VkBool32 presentSupport = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
    if (presentSupport == VK_TRUE) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool VulkanRenderer::checkDeviceExtensionSupport(VkPhysicalDevice device) {
  uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       nullptr);
  std::vector<VkExtensionProperties> available(extensionCount);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       available.data());

  std::set<std::string> required(kDeviceExtensions.begin(),
                                 kDeviceExtensions.end());
  for (const auto &ext : available) {
    required.erase(ext.extensionName);
  }
  return required.empty();
}

VulkanRenderer::SwapchainSupportDetails
VulkanRenderer::querySwapchainSupport(VkPhysicalDevice device) {
  SwapchainSupportDetails details{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface,
                                            &details.capabilities);

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount,
                                       nullptr);
  details.formats.resize(formatCount);
  if (formatCount) {
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount,
                                         details.formats.data());
  }

  uint32_t presentModeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface,
                                            &presentModeCount, nullptr);
  details.presentModes.resize(presentModeCount);
  if (presentModeCount) {
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, m_surface, &presentModeCount, details.presentModes.data());
  }

  return details;
}

bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) {
  const uint32_t graphicsFamily = findGraphicsQueueFamily(device);
  const uint32_t presentFamily = findPresentQueueFamily(device);
  if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
    return false;
  }

  if (!checkDeviceExtensionSupport(device)) {
    return false;
  }

  const auto swapSupport = querySwapchainSupport(device);
  if (swapSupport.formats.empty() || swapSupport.presentModes.empty()) {
    return false;
  }

  m_graphicsQueueFamily = graphicsFamily;
  m_presentQueueFamily = presentFamily;
  return true;
}

bool VulkanRenderer::pickPhysicalDevice(std::string &outError) {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
  if (deviceCount == 0) {
    outError =
        "No Vulkan-capable GPU found (vkEnumeratePhysicalDevices returned 0).";
    return false;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

  for (VkPhysicalDevice device : devices) {
    if (isDeviceSuitable(device)) {
      m_physicalDevice = device;
      return true;
    }
  }

  outError = "No suitable Vulkan device found (requires graphics + present "
             "queue and VK_KHR_swapchain).";
  return false;
}

bool VulkanRenderer::createDevice(std::string &outError) {
  std::set<uint32_t> uniqueFamilies = {m_graphicsQueueFamily,
                                       m_presentQueueFamily};

  float queuePriority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queueInfos;
  queueInfos.reserve(uniqueFamilies.size());
  for (uint32_t family : uniqueFamilies) {
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &queuePriority;
    queueInfos.push_back(q);
  }

  VkPhysicalDeviceFeatures features{};

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
  createInfo.pQueueCreateInfos = queueInfos.data();
  createInfo.pEnabledFeatures = &features;
  createInfo.enabledExtensionCount =
      static_cast<uint32_t>(kDeviceExtensions.size());
  createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();

  VkResult r =
      vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
  if (r != VK_SUCCESS) {
    outError = "Failed to create logical device (" + vkResultToString(r) + ").";
    return false;
  }

  vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
  vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);
  return true;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &formats) {
  auto find = [&](VkFormat fmt) -> std::optional<VkSurfaceFormatKHR> {
    for (const auto &f : formats) {
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

  // Fall back to common formats; we'll later fail with a clear error if we
  // can't run compute on them.
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

VkPresentModeKHR
VulkanRenderer::choosePresentMode(const std::vector<VkPresentModeKHR> &modes) {
  for (auto m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
      return m;
    }
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities,
                                 uint32_t width, uint32_t height) {
  if (capabilities.currentExtent.width != UINT32_MAX) {
    return capabilities.currentExtent;
  }

  VkExtent2D actual{};
  actual.width = clampU32(width, capabilities.minImageExtent.width,
                          capabilities.maxImageExtent.width);
  actual.height = clampU32(height, capabilities.minImageExtent.height,
                           capabilities.maxImageExtent.height);
  return actual;
}

bool VulkanRenderer::createSwapchain(uint32_t width, uint32_t height,
                                     std::string &outError) {
  const SwapchainSupportDetails support =
      querySwapchainSupport(m_physicalDevice);
  const VkSurfaceFormatKHR surfaceFormat =
      chooseSwapSurfaceFormat(support.formats);
  const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
  const VkExtent2D extent =
      chooseSwapExtent(support.capabilities, width, height);

  uint32_t imageCount = support.capabilities.minImageCount + 1;
  if (support.capabilities.maxImageCount > 0 &&
      imageCount > support.capabilities.maxImageCount) {
    imageCount = support.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = m_surface;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  uint32_t queueFamilyIndices[] = {m_graphicsQueueFamily, m_presentQueueFamily};
  if (m_graphicsQueueFamily != m_presentQueueFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = support.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = VK_NULL_HANDLE;

  VkResult r =
      vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain);
  if (r != VK_SUCCESS) {
    outError = "Failed to create swapchain (" + vkResultToString(r) + ").";
    return false;
  }

  uint32_t actualCount = 0;
  vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualCount, nullptr);
  m_swapchainImages.resize(actualCount);
  vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualCount,
                          m_swapchainImages.data());
  m_swapchainImageLayouts.assign(m_swapchainImages.size(),
                                 VK_IMAGE_LAYOUT_UNDEFINED);

  m_swapchainFormat = surfaceFormat.format;
  m_swapchainExtent = extent;
  if (m_swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM &&
      m_swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB &&
      m_swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM &&
      m_swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB) {
    outError = "Unsupported swapchain format for voxel renderer (expected "
               "BGRA8/RGBA8 UNORM or SRGB).";
    return false;
  }

  m_swapchainImageViews.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
  for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_swapchainImages[i];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_swapchainFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    r = vkCreateImageView(m_device, &viewInfo, nullptr,
                          &m_swapchainImageViews[i]);
    if (r != VK_SUCCESS) {
      outError = "Failed to create image view (" + vkResultToString(r) + ").";
      return false;
    }
  }

  if (!createStorageResources(outError) || !createDescriptorSet(outError) ||
      !createComputePipeline(outError)) {
    return false;
  }

  return true;
}

void VulkanRenderer::cleanupSwapchain() {
  cleanupComputePipeline();
  cleanupDescriptorSet();
  cleanupStorageResources();

  for (auto iv : m_swapchainImageViews) {
    vkDestroyImageView(m_device, iv, nullptr);
  }
  m_swapchainImageViews.clear();

  if (m_swapchain) {
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
  }
  m_swapchainImages.clear();
  m_swapchainImageLayouts.clear();
}

bool VulkanRenderer::recreateSwapchain(uint32_t width, uint32_t height,
                                       std::string &outError) {
  if (width == 0 || height == 0) {
    return true;
  }

  vkDeviceWaitIdle(m_device);
  cleanupSwapchain();

  return createSwapchain(width, height, outError);
}

bool VulkanRenderer::createDescriptorSetLayout(std::string &outError) {
  (void)outError;

  VkDescriptorSetLayoutBinding voxelBufferBinding{};
  voxelBufferBinding.binding = 0;
  voxelBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  voxelBufferBinding.descriptorCount = 1;
  voxelBufferBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding outputBufferBinding{};
  outputBufferBinding.binding = 1;
  outputBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  outputBufferBinding.descriptorCount = 1;
  outputBufferBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding sceneBinding{};
  sceneBinding.binding = 2;
  sceneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sceneBinding.descriptorCount = 1;
  sceneBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {voxelBufferBinding,
                                             outputBufferBinding, sceneBinding};

  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = 3;
  info.pBindings = bindings;

  VkResult r = vkCreateDescriptorSetLayout(m_device, &info, nullptr,
                                           &m_descriptorSetLayout);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create descriptor set layout (" + vkResultToString(r) + ").";
    return false;
  }

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push.offset = 0;
  push.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &m_descriptorSetLayout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &push;

  r = vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create pipeline layout (" + vkResultToString(r) + ").";
    return false;
  }

  return true;
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<char> &code,
                                                  std::string &outError) {
  if (code.empty() || (code.size() % 4) != 0) {
    outError = "Invalid SPIR-V shader binary (empty or not aligned).";
    return VK_NULL_HANDLE;
  }

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

  VkShaderModule module = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(m_device, &createInfo, nullptr, &module);
  if (r != VK_SUCCESS) {
    outError = "Failed to create shader module (" + vkResultToString(r) + ").";
    return VK_NULL_HANDLE;
  }
  return module;
}

bool VulkanRenderer::createVoxelWorldAndUpload(std::string &outError) {
  vv::voxel::World world(vv::voxel::Extent3u{
      m_chunkSizeVoxels.x, m_chunkSizeVoxels.y, m_chunkSizeVoxels.z});
  const vv::voxel::Chunk &chunk = world.chunk0();

  const auto &voxels = chunk.rawVoxelsU32();
  const VkDeviceSize voxelBytes =
      static_cast<VkDeviceSize>(voxels.size() * sizeof(uint32_t));
  if (voxelBytes == 0) {
    outError = "World produced an empty chunk.";
    return false;
  }

  VkResult r = VK_SUCCESS;
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;

  auto cleanupTemp = [&]() {
    if (cmd) {
      vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
      cmd = VK_NULL_HANDLE;
    }
    if (stagingBuffer) {
      vkDestroyBuffer(m_device, stagingBuffer, nullptr);
      stagingBuffer = VK_NULL_HANDLE;
    }
    if (stagingMemory) {
      vkFreeMemory(m_device, stagingMemory, nullptr);
      stagingMemory = VK_NULL_HANDLE;
    }
  };

  VkBufferCreateInfo voxelBuf{};
  voxelBuf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  voxelBuf.size = voxelBytes;
  voxelBuf.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  voxelBuf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  r = vkCreateBuffer(m_device, &voxelBuf, nullptr, &m_voxelBuffer);
  if (r != VK_SUCCESS) {
    outError = "Failed to create voxel buffer (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkMemoryRequirements voxelReq{};
  vkGetBufferMemoryRequirements(m_device, m_voxelBuffer, &voxelReq);
  uint32_t voxelMemType =
      findMemoryTypeIndex(m_physicalDevice, voxelReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (voxelMemType == UINT32_MAX) {
    outError = "No suitable device-local memory type found for voxel buffer.";
    cleanupTemp();
    return false;
  }

  VkMemoryAllocateInfo voxelAlloc{};
  voxelAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  voxelAlloc.allocationSize = voxelReq.size;
  voxelAlloc.memoryTypeIndex = voxelMemType;

  r = vkAllocateMemory(m_device, &voxelAlloc, nullptr, &m_voxelBufferMemory);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to allocate voxel buffer memory (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  r = vkBindBufferMemory(m_device, m_voxelBuffer, m_voxelBufferMemory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind voxel buffer memory (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkBufferCreateInfo stagingInfo{};
  stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingInfo.size = voxelBytes;
  stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  r = vkCreateBuffer(m_device, &stagingInfo, nullptr, &stagingBuffer);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create voxel staging buffer (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkMemoryRequirements stagingReq{};
  vkGetBufferMemoryRequirements(m_device, stagingBuffer, &stagingReq);
  uint32_t stagingType =
      findMemoryTypeIndex(m_physicalDevice, stagingReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (stagingType == UINT32_MAX) {
    outError =
        "No suitable host-visible memory type found for voxel staging buffer.";
    cleanupTemp();
    return false;
  }

  VkMemoryAllocateInfo stagingAlloc{};
  stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  stagingAlloc.allocationSize = stagingReq.size;
  stagingAlloc.memoryTypeIndex = stagingType;

  r = vkAllocateMemory(m_device, &stagingAlloc, nullptr, &stagingMemory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate voxel staging memory (" +
               vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  r = vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind voxel staging memory (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  void *mapped = nullptr;
  r = vkMapMemory(m_device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
  if (r != VK_SUCCESS || !mapped) {
    outError = "Failed to map voxel staging memory.";
    cleanupTemp();
    return false;
  }
  std::memcpy(mapped, voxels.data(), static_cast<size_t>(voxelBytes));
  vkUnmapMemory(m_device, stagingMemory);
  VkCommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAlloc.commandPool = m_commandPool;
  cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAlloc.commandBufferCount = 1;

  r = vkAllocateCommandBuffers(m_device, &cmdAlloc, &cmd);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate voxel upload command buffer (" +
               vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
    outError = "Failed to begin voxel upload command buffer.";
    cleanupTemp();
    return false;
  }

  VkBufferCopy copy{};
  copy.srcOffset = 0;
  copy.dstOffset = 0;
  copy.size = voxelBytes;
  vkCmdCopyBuffer(cmd, stagingBuffer, m_voxelBuffer, 1, &copy);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    outError = "Failed to end voxel upload command buffer.";
    cleanupTemp();
    return false;
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  r = vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) {
    outError = "Failed to submit voxel upload (" + vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }
  vkQueueWaitIdle(m_graphicsQueue);

  cleanupTemp();
  return true;
}

void VulkanRenderer::cleanupVoxelResources() {
  if (m_voxelBuffer) {
    vkDestroyBuffer(m_device, m_voxelBuffer, nullptr);
    m_voxelBuffer = VK_NULL_HANDLE;
  }
  if (m_voxelBufferMemory) {
    vkFreeMemory(m_device, m_voxelBufferMemory, nullptr);
    m_voxelBufferMemory = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::createSceneResources(std::string &outError) {
  VkBufferCreateInfo buf{};
  buf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf.size = static_cast<VkDeviceSize>(sizeof(SceneUBO));
  buf.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  buf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult r = vkCreateBuffer(m_device, &buf, nullptr, &m_sceneBuffer);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create scene uniform buffer (" + vkResultToString(r) + ").";
    return false;
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(m_device, m_sceneBuffer, &req);
  uint32_t memType =
      findMemoryTypeIndex(m_physicalDevice, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memType == UINT32_MAX) {
    outError =
        "No suitable host-visible memory type found for scene uniform buffer.";
    return false;
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memType;

  r = vkAllocateMemory(m_device, &alloc, nullptr, &m_sceneBufferMemory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate scene uniform buffer memory (" +
               vkResultToString(r) + ").";
    return false;
  }

  r = vkBindBufferMemory(m_device, m_sceneBuffer, m_sceneBufferMemory, 0);
  if (r != VK_SUCCESS) {
    outError = "Failed to bind scene uniform buffer memory (" +
               vkResultToString(r) + ").";
    return false;
  }

  void *mapped = nullptr;
  r = vkMapMemory(m_device, m_sceneBufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
  if (r != VK_SUCCESS || !mapped) {
    outError = "Failed to map scene uniform buffer memory.";
    return false;
  }

  m_sceneBufferMapped = mapped;
  std::memset(m_sceneBufferMapped, 0, sizeof(SceneUBO));
  return true;
}

void VulkanRenderer::cleanupSceneResources() {
  if (m_sceneBufferMapped) {
    vkUnmapMemory(m_device, m_sceneBufferMemory);
    m_sceneBufferMapped = nullptr;
  }
  if (m_sceneBuffer) {
    vkDestroyBuffer(m_device, m_sceneBuffer, nullptr);
    m_sceneBuffer = VK_NULL_HANDLE;
  }
  if (m_sceneBufferMemory) {
    vkFreeMemory(m_device, m_sceneBufferMemory, nullptr);
    m_sceneBufferMemory = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::createStorageResources(std::string &outError) {
  const uint64_t elements = static_cast<uint64_t>(m_swapchainExtent.width) *
                            static_cast<uint64_t>(m_swapchainExtent.height);
  const VkDeviceSize bufferSize =
      static_cast<VkDeviceSize>(elements * sizeof(uint32_t));

  VkResult r = VK_SUCCESS;

  VkBufferCreateInfo outBuf{};
  outBuf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  outBuf.size = bufferSize;
  outBuf.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  outBuf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  r = vkCreateBuffer(m_device, &outBuf, nullptr, &m_outputBuffer);
  if (r != VK_SUCCESS) {
    outError = "Failed to create output buffer (" + vkResultToString(r) + ").";
    return false;
  }

  VkMemoryRequirements outReq{};
  vkGetBufferMemoryRequirements(m_device, m_outputBuffer, &outReq);
  uint32_t outMemType =
      findMemoryTypeIndex(m_physicalDevice, outReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (outMemType == UINT32_MAX) {
    // Fallback: still works, just slower.
    outMemType = findMemoryTypeIndex(m_physicalDevice, outReq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  if (outMemType == UINT32_MAX) {
    outError = "No suitable memory type found for output buffer.";
    return false;
  }

  VkMemoryAllocateInfo outAlloc{};
  outAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  outAlloc.allocationSize = outReq.size;
  outAlloc.memoryTypeIndex = outMemType;

  r = vkAllocateMemory(m_device, &outAlloc, nullptr, &m_outputBufferMemory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate output buffer memory (" +
               vkResultToString(r) + ").";
    return false;
  }

  r = vkBindBufferMemory(m_device, m_outputBuffer, m_outputBufferMemory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind output buffer memory (" + vkResultToString(r) + ").";
    return false;
  }

  return true;
}

void VulkanRenderer::cleanupStorageResources() {
  if (m_outputBuffer) {
    vkDestroyBuffer(m_device, m_outputBuffer, nullptr);
    m_outputBuffer = VK_NULL_HANDLE;
  }
  if (m_outputBufferMemory) {
    vkFreeMemory(m_device, m_outputBufferMemory, nullptr);
    m_outputBufferMemory = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::createDescriptorSet(std::string &outError) {
  VkDescriptorPoolSize poolSizes[3] = {};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 1;
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[1].descriptorCount = 1;
  poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[2].descriptorCount = 1;

  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = 3;
  pool.pPoolSizes = poolSizes;

  VkResult r =
      vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create descriptor pool (" + vkResultToString(r) + ").";
    return false;
  }

  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = m_descriptorPool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &m_descriptorSetLayout;

  r = vkAllocateDescriptorSets(m_device, &alloc, &m_descriptorSet);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to allocate descriptor set (" + vkResultToString(r) + ").";
    return false;
  }

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = m_voxelBuffer;
  bufferInfo.offset = 0;
  bufferInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo outBufferInfo{};
  outBufferInfo.buffer = m_outputBuffer;
  outBufferInfo.offset = 0;
  outBufferInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo sceneInfo{};
  sceneInfo.buffer = m_sceneBuffer;
  sceneInfo.offset = 0;
  sceneInfo.range = sizeof(SceneUBO);

  VkWriteDescriptorSet writes[3] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = m_descriptorSet;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &bufferInfo;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = m_descriptorSet;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outBufferInfo;

  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = m_descriptorSet;
  writes[2].dstBinding = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[2].pBufferInfo = &sceneInfo;

  vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
  return true;
}

void VulkanRenderer::cleanupDescriptorSet() {
  m_descriptorSet = VK_NULL_HANDLE;
  if (m_descriptorPool) {
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::createComputePipeline(std::string &outError) {
  const auto shaderDir = vv::core::executableDir() / "resources" / "shaders";

  const auto compPath = shaderDir / "pixels_rgba.comp.spv";

  std::string compErr;
  std::vector<char> compCode = vv::core::loadBinaryFile(compPath, compErr);
  if (compCode.empty()) {
    outError = compErr;
    return false;
  }

  VkShaderModule compModule = createShaderModule(compCode, outError);
  if (!compModule) {
    return false;
  }

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName = "main";

  VkComputePipelineCreateInfo pipe{};
  pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipe.stage = stage;
  pipe.layout = m_pipelineLayout;

  VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipe,
                                        nullptr, &m_computePipeline);
  vkDestroyShaderModule(m_device, compModule, nullptr);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create compute pipeline (" + vkResultToString(r) + ").";
    return false;
  }

  return true;
}

void VulkanRenderer::cleanupComputePipeline() {
  if (m_computePipeline) {
    vkDestroyPipeline(m_device, m_computePipeline, nullptr);
    m_computePipeline = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::createCommandPool(std::string &outError) {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

  VkResult r =
      vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
  if (r != VK_SUCCESS) {
    outError = "Failed to create command pool (" + vkResultToString(r) + ").";
    return false;
  }
  return true;
}

bool VulkanRenderer::createCommandBuffers(std::string &outError) {
  m_commandBuffers.resize(kMaxFramesInFlight, VK_NULL_HANDLE);

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = m_commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

  VkResult r =
      vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data());
  if (r != VK_SUCCESS) {
    outError =
        "Failed to allocate command buffers (" + vkResultToString(r) + ").";
    return false;
  }

  return true;
}

bool VulkanRenderer::createSyncObjects(std::string &outError) {
  m_imageAvailableSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
  m_renderFinishedSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
  m_inFlightFences.resize(kMaxFramesInFlight, VK_NULL_HANDLE);

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    VkResult r1 = vkCreateSemaphore(m_device, &semInfo, nullptr,
                                    &m_imageAvailableSemaphores[i]);
    VkResult r2 = vkCreateSemaphore(m_device, &semInfo, nullptr,
                                    &m_renderFinishedSemaphores[i]);
    VkResult r3 =
        vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]);
    if (r1 != VK_SUCCESS || r2 != VK_SUCCESS || r3 != VK_SUCCESS) {
      outError = "Failed to create synchronization objects.";
      return false;
    }
  }
  return true;
}

bool VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd,
                                         uint32_t imageIndex,
                                         std::string &outError) {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    outError = "vkBeginCommandBuffer failed.";
    return false;
  }

  VkBufferMemoryBarrier preComputeBarriers[3] = {};
  preComputeBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preComputeBarriers[0].srcAccessMask = 0;
  preComputeBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  preComputeBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[0].buffer = m_voxelBuffer;
  preComputeBarriers[0].offset = 0;
  preComputeBarriers[0].size = VK_WHOLE_SIZE;

  preComputeBarriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preComputeBarriers[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preComputeBarriers[1].dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
  preComputeBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[1].buffer = m_sceneBuffer;
  preComputeBarriers[1].offset = 0;
  preComputeBarriers[1].size = VK_WHOLE_SIZE;

  preComputeBarriers[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preComputeBarriers[2].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  preComputeBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  preComputeBarriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[2].buffer = m_outputBuffer;
  preComputeBarriers[2].offset = 0;
  preComputeBarriers[2].size = VK_WHOLE_SIZE;

  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 3,
                       preComputeBarriers, 0, nullptr);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                          0, 1, &m_descriptorSet, 0, nullptr);

  const uint32_t isBgra = (m_swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                           m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB)
                              ? 1u
                              : 0u;
  PushConstants push{};
  push.screen = glm::uvec4(m_swapchainExtent.width, m_swapchainExtent.height,
                           isBgra, m_frameCounter);
  push.camera = glm::vec4(m_camera.tanHalfFovRadians(), 0.0f, 0.0f, 0.0f);
  push.chunkSize = glm::uvec4(m_chunkSizeVoxels, 0u);
  push.voxelSize = glm::vec4(m_voxelSize, 0.0f);
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), &push);

  const uint32_t groupX = (m_swapchainExtent.width + 15u) / 16u;
  const uint32_t groupY = (m_swapchainExtent.height + 15u) / 16u;
  vkCmdDispatch(cmd, groupX, groupY, 1);

  VkBufferMemoryBarrier outToTransfer{};
  outToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  outToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  outToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  outToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  outToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  outToTransfer.buffer = m_outputBuffer;
  outToTransfer.offset = 0;
  outToTransfer.size = VK_WHOLE_SIZE;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                       &outToTransfer, 0, nullptr);

  VkImageMemoryBarrier swapToDst{};
  swapToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  swapToDst.srcAccessMask = 0;
  swapToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  swapToDst.oldLayout = m_swapchainImageLayouts[imageIndex];
  swapToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  swapToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  swapToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  swapToDst.image = m_swapchainImages[imageIndex];
  swapToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  swapToDst.subresourceRange.baseMipLevel = 0;
  swapToDst.subresourceRange.levelCount = 1;
  swapToDst.subresourceRange.baseArrayLayer = 0;
  swapToDst.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &swapToDst);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {m_swapchainExtent.width, m_swapchainExtent.height, 1};

  vkCmdCopyBufferToImage(cmd, m_outputBuffer, m_swapchainImages[imageIndex],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier swapToPresent{};
  swapToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  swapToPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  swapToPresent.dstAccessMask = 0;
  swapToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  swapToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  swapToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  swapToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  swapToPresent.image = m_swapchainImages[imageIndex];
  swapToPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  swapToPresent.subresourceRange.baseMipLevel = 0;
  swapToPresent.subresourceRange.levelCount = 1;
  swapToPresent.subresourceRange.baseArrayLayer = 0;
  swapToPresent.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &swapToPresent);

  m_swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    outError = "vkEndCommandBuffer failed.";
    return false;
  }

  return true;
}

} // namespace vv::vulkan
