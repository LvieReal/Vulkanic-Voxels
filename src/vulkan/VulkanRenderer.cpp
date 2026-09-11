#include "vulkan/VulkanRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <set>

#include "core/RuntimePaths.hpp"
#include "core/ShaderLoader.hpp"
#include "platform/VulkanSurfaceFactory.hpp"
#include "render/SceneData.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace vv::vulkan {

namespace {
using namespace vv::render;
using namespace vv::vulkan::utils;
} // namespace

VulkanRenderer::~VulkanRenderer() {
  cleanup();
}

bool VulkanRenderer::init(const InitInfo& info, std::string& outError) {
  if (m_initialized) {
    return true;
  }
  if (!info.nativeWindow.isValid()) {
    outError =
        "Invalid native window handle (platform backend not resolved).";
    return false;
  }

  if (!createInstance(info, outError) || !createSurface(info, outError) ||
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

  // Delegated to SceneUniform utility: updates camera + lighting UBO.
  m_sceneUniform.update(m_camera, m_timeSeconds, m_lighting);

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

void VulkanRenderer::setCamera(const vv::core::Camera& camera,
                               float timeSeconds) {
  m_camera = camera;
  m_timeSeconds = timeSeconds;
}

void VulkanRenderer::setWorldConfig(const glm::uvec3& chunkSizeVoxels,
                                    const glm::vec3& voxelSize) {
  if (m_initialized) {
    return;
  }

  m_voxelConfig.chunkSizeVoxels =
      glm::max(chunkSizeVoxels, glm::uvec3(1u, 1u, 1u));
  m_voxelConfig.voxelSize =
      glm::max(voxelSize, glm::vec3(1e-3f, 1e-3f, 1e-3f));
}

bool VulkanRenderer::createInstance(const InitInfo& info,
                                   std::string& outError) {
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  auto* fpEnumerateInstanceVersion =
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

  // Platform-delegated: the WSI extensions matching the native window kind
  // (VK_KHR_win32_surface / VK_KHR_xcb_surface / VK_KHR_wayland_surface /
  // VK_MVK_macos_surface), always preceded by VK_KHR_surface.
  const std::vector<const char*> extensions =
      vv::platform::requiredVulkanInstanceExtensions(info.nativeWindow);

  // Only request extensions the loader actually supports, and fail with a
  // clear message when a required WSI extension is missing.
  uint32_t availableCount = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
  std::vector<VkExtensionProperties> available(availableCount);
  if (availableCount > 0) {
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount,
                                           available.data());
  }
  for (const char* extension : extensions) {
    const bool supported = std::any_of(
        available.begin(), available.end(), [extension](const auto& props) {
          return std::strcmp(props.extensionName, extension) == 0;
        });
    if (!supported) {
      outError = std::string("Required Vulkan instance extension '") +
                 extension +
                 "' is not supported by the Vulkan loader on this system.";
      return false;
    }
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  VkResult r = vkCreateInstance(&createInfo, nullptr, &m_instance);
  if (r != VK_SUCCESS) {
    outError = "Failed to create Vulkan instance (" +
               utils::vkResultToString(r) +
               "). Vulkan might be missing or unsupported on this system.";
    return false;
  }
  return true;
}

bool VulkanRenderer::createSurface(const InitInfo& info,
                                   std::string& outError) {
  // Platform-delegated: Win32 / XCB / Wayland / MoltenVK surface creation is
  // selected from the native window kind (see platform/VulkanSurfaceFactory).
  return vv::platform::createVulkanSurface(m_instance, info.nativeWindow,
                                           &m_surface, outError);
}

bool VulkanRenderer::pickPhysicalDevice(std::string& outError) {
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
    uint32_t gfx = UINT32_MAX, present = UINT32_MAX;
    if (utils::isDeviceSuitable(device, m_surface, gfx, present)) {
      m_physicalDevice = device;
      m_graphicsQueueFamily = gfx;
      m_presentQueueFamily = present;
      return true;
    }
  }

  outError =
      "No suitable Vulkan device found (requires graphics + present "
      "queue and VK_KHR_swapchain).";
  return false;
}

bool VulkanRenderer::createDevice(std::string& outError) {
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
      static_cast<uint32_t>(utils::kDeviceExtensions.size());
  createInfo.ppEnabledExtensionNames = utils::kDeviceExtensions.data();

  VkResult r =
      vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
  if (r != VK_SUCCESS) {
    outError = "Failed to create logical device (" + utils::vkResultToString(r) + ").";
    return false;
  }

  vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
  vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);
  return true;
}

bool VulkanRenderer::createSwapchain(uint32_t width, uint32_t height,
                                     std::string& outError) {
  const utils::SwapchainSupportDetails support =
      utils::querySwapchainSupport(m_physicalDevice, m_surface);
  const VkSurfaceFormatKHR surfaceFormat =
      utils::chooseSwapSurfaceFormat(support.formats);
  const VkPresentModeKHR presentMode =
      utils::choosePresentMode(support.presentModes);
  const VkExtent2D extent =
      utils::chooseSwapExtent(support.capabilities, width, height);

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
    outError = "Failed to create swapchain (" + utils::vkResultToString(r) + ").";
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
    outError =
        "Unsupported swapchain format for voxel renderer (expected "
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
      outError = "Failed to create image view (" + utils::vkResultToString(r) + ").";
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
                                       std::string& outError) {
  if (width == 0 || height == 0) {
    return true;
  }

  vkDeviceWaitIdle(m_device);
  cleanupSwapchain();

  return createSwapchain(width, height, outError);
}

bool VulkanRenderer::createDescriptorSetLayout(std::string& outError) {
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
        "Failed to create descriptor set layout (" + utils::vkResultToString(r) + ").";
    return false;
  }

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push.offset = 0;
  push.size = sizeof(vv::render::PushConstants);

  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &m_descriptorSetLayout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &push;

  r = vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create pipeline layout (" + utils::vkResultToString(r) + ").";
    return false;
  }

  return true;
}

bool VulkanRenderer::createVoxelWorldAndUpload(std::string& outError) {
  // Delegated to VoxelResources utility (voxel domain separated from renderer)
  return m_voxelResources.createAndUpload(
      m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
      m_voxelConfig, outError);
}

void VulkanRenderer::cleanupVoxelResources() {
  m_voxelResources.cleanup(m_device);
}

bool VulkanRenderer::createSceneResources(std::string& outError) {
  // Delegated to SceneUniform utility (camera/lighting separated)
  return m_sceneUniform.create(m_device, m_physicalDevice, outError);
}

void VulkanRenderer::cleanupSceneResources() {
  m_sceneUniform.cleanup(m_device);
}

bool VulkanRenderer::createStorageResources(std::string& outError) {
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
    outError = "Failed to create output buffer (" + utils::vkResultToString(r) + ").";
    return false;
  }

  VkMemoryRequirements outReq{};
  vkGetBufferMemoryRequirements(m_device, m_outputBuffer, &outReq);
  uint32_t outMemType = utils::findMemoryTypeIndex(
      m_physicalDevice, outReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (outMemType == UINT32_MAX) {
    outMemType = utils::findMemoryTypeIndex(
        m_physicalDevice, outReq.memoryTypeBits,
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
               utils::vkResultToString(r) + ").";
    return false;
  }

  r = vkBindBufferMemory(m_device, m_outputBuffer, m_outputBufferMemory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind output buffer memory (" + utils::vkResultToString(r) + ").";
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

bool VulkanRenderer::createDescriptorSet(std::string& outError) {
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
        "Failed to create descriptor pool (" + utils::vkResultToString(r) + ").";
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
        "Failed to allocate descriptor set (" + utils::vkResultToString(r) + ").";
    return false;
  }

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = m_voxelResources.buffer();
  bufferInfo.offset = 0;
  bufferInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo outBufferInfo{};
  outBufferInfo.buffer = m_outputBuffer;
  outBufferInfo.offset = 0;
  outBufferInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo sceneInfo{};
  sceneInfo.buffer = m_sceneUniform.buffer();
  sceneInfo.offset = 0;
  sceneInfo.range = sizeof(vv::render::SceneUBO);

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

bool VulkanRenderer::createComputePipeline(std::string& outError) {
  const auto shaderDir = vv::core::executableDir() / "resources" / "shaders";

  const auto compPath = shaderDir / "pixels_rgba.comp.spv";

  std::string compErr;
  std::vector<char> compCode = vv::core::loadBinaryFile(compPath, compErr);
  if (compCode.empty()) {
    outError = compErr;
    return false;
  }

  VkShaderModule compModule =
      utils::createShaderModule(m_device, compCode, outError);
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
        "Failed to create compute pipeline (" + utils::vkResultToString(r) + ").";
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

bool VulkanRenderer::createCommandPool(std::string& outError) {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

  VkResult r =
      vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
  if (r != VK_SUCCESS) {
    outError = "Failed to create command pool (" + utils::vkResultToString(r) + ").";
    return false;
  }
  return true;
}

bool VulkanRenderer::createCommandBuffers(std::string& outError) {
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
        "Failed to allocate command buffers (" + utils::vkResultToString(r) + ").";
    return false;
  }

  return true;
}

bool VulkanRenderer::createSyncObjects(std::string& outError) {
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
                                         std::string& outError) {
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
  preComputeBarriers[0].buffer = m_voxelResources.buffer();
  preComputeBarriers[0].offset = 0;
  preComputeBarriers[0].size = VK_WHOLE_SIZE;

  preComputeBarriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preComputeBarriers[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preComputeBarriers[1].dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
  preComputeBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[1].buffer = m_sceneUniform.buffer();
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
  vv::render::PushConstants push{};
  push.screen = glm::uvec4(m_swapchainExtent.width, m_swapchainExtent.height,
                           isBgra, m_frameCounter);
  push.camera = glm::vec4(m_camera.tanHalfFovRadians(), 0.0f, 0.0f, 0.0f);
  push.chunkSize = glm::uvec4(m_voxelConfig.chunkSizeVoxels, 0u);
  push.voxelSize = glm::vec4(m_voxelConfig.voxelSize, 0.0f);
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
