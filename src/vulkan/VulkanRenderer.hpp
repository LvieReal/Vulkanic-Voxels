#pragma once

#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/Camera.hpp"
#include "render/LightingConfig.hpp"
#include "render/SceneUniform.hpp"
#include "voxel/VoxelConfig.hpp"
#include "vulkan/VoxelResources.hpp"

namespace vv::vulkan {

class VulkanRenderer final {
 public:
  struct InitInfo {
    HINSTANCE hinstance = nullptr;
    HWND hwnd = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  VulkanRenderer() = default;
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  bool init(const InitInfo& info, std::string& outError);
  void resize(uint32_t width, uint32_t height);
  void drawFrame();
  void setCamera(const vv::core::Camera& camera, float timeSeconds);
  void setWorldConfig(const glm::uvec3& chunkSizeVoxels,
                      const glm::vec3& voxelSize);

  // New: allow external configuration of lighting (separated concern).
  void setLighting(const vv::render::LightingConfig& lighting) {
    m_lighting = lighting;
  }
  const vv::render::LightingConfig& lighting() const { return m_lighting; }
  const vv::voxel::VoxelConfig& voxelConfig() const { return m_voxelConfig; }

 private:
  void cleanup();

  bool createInstance(std::string& outError);
  bool createSurface(const InitInfo& info, std::string& outError);
  bool pickPhysicalDevice(std::string& outError);
  bool createDevice(std::string& outError);

  bool createSwapchain(uint32_t width, uint32_t height, std::string& outError);
  void cleanupSwapchain();
  bool recreateSwapchain(uint32_t width, uint32_t height,
                         std::string& outError);

  bool createVoxelWorldAndUpload(std::string& outError);
  void cleanupVoxelResources();

  bool createSceneResources(std::string& outError);
  void cleanupSceneResources();

  bool createStorageResources(std::string& outError);
  void cleanupStorageResources();

  bool createDescriptorSetLayout(std::string& outError);
  bool createDescriptorSet(std::string& outError);
  void cleanupDescriptorSet();

  bool createComputePipeline(std::string& outError);
  void cleanupComputePipeline();

  bool createCommandPool(std::string& outError);
  bool createCommandBuffers(std::string& outError);
  bool createSyncObjects(std::string& outError);

  bool recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                           std::string& outError);

 private:
  // --- Core Vulkan context (renderer's true responsibility) ---
  VkInstance m_instance = VK_NULL_HANDLE;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  VkQueue m_presentQueue = VK_NULL_HANDLE;
  uint32_t m_graphicsQueueFamily = UINT32_MAX;
  uint32_t m_presentQueueFamily = UINT32_MAX;

  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D m_swapchainExtent{};
  std::vector<VkImage> m_swapchainImages;
  std::vector<VkImageView> m_swapchainImageViews;
  std::vector<VkImageLayout> m_swapchainImageLayouts;

  // Output storage image (compute -> swapchain copy)
  VkBuffer m_outputBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_outputBufferMemory = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_computePipeline = VK_NULL_HANDLE;

  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  static constexpr uint32_t kMaxFramesInFlight = 2;
  std::vector<VkCommandBuffer> m_commandBuffers;

  std::vector<VkSemaphore> m_imageAvailableSemaphores;
  std::vector<VkSemaphore> m_renderFinishedSemaphores;
  std::vector<VkFence> m_inFlightFences;
  uint32_t m_currentFrame = 0;
  uint32_t m_frameCounter = 0;

  bool m_initialized = false;
  bool m_framebufferResized = false;

  // --- Separated concerns (delegated to utilities / config objects) ---

  // Camera is frame-varying state; stored here for convenience but
  // update logic is delegated to SceneUniform utility (see render/SceneUniform).
  vv::core::Camera m_camera;
  float m_timeSeconds = 0.0f;

  // World voxel configuration separated into VoxelConfig utility.
  vv::voxel::VoxelConfig m_voxelConfig{};

  // Voxel storage separated into VoxelResources utility class.
  vv::vulkan::VoxelResources m_voxelResources;

  // Scene uniform (camera + lighting) separated into SceneUniform utility.
  vv::render::SceneUniform m_sceneUniform;

  // Lighting properties separated into LightingConfig utility.
  vv::render::LightingConfig m_lighting{};
};

} // namespace vv::vulkan
