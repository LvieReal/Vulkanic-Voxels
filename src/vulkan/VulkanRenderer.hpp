#pragma once

#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/Camera.hpp"

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

	struct SwapchainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities{};
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device);
	bool checkDeviceExtensionSupport(VkPhysicalDevice device);
	bool isDeviceSuitable(VkPhysicalDevice device);

	VkSurfaceFormatKHR
	chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
	VkPresentModeKHR
	choosePresentMode(const std::vector<VkPresentModeKHR>& modes);
	VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
															uint32_t width, uint32_t height);

	uint32_t findGraphicsQueueFamily(VkPhysicalDevice device);
	uint32_t findPresentQueueFamily(VkPhysicalDevice device);

	VkShaderModule createShaderModule(const std::vector<char>& code,
																		std::string& outError);

 private:
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

	VkBuffer m_voxelBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_voxelBufferMemory = VK_NULL_HANDLE;

	VkBuffer m_sceneBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_sceneBufferMemory = VK_NULL_HANDLE;
	void* m_sceneBufferMapped = nullptr;

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

	vv::core::Camera m_camera;
	float m_timeSeconds = 0.0f;
	glm::uvec3 m_chunkSizeVoxels = glm::uvec3(64u, 64u, 64u);
	glm::vec3 m_voxelSize = glm::vec3(1.0f, 1.0f, 1.0f);
	glm::vec3 m_lightDir = glm::vec3(0.3f, 0.9f, 0.2f);
	glm::vec3 m_lightColor = glm::vec3(1.0f);
	glm::vec3 m_skyLow = glm::vec3(0.05f, 0.08f, 0.12f);
	glm::vec3 m_skyHigh = glm::vec3(0.2f, 0.3f, 0.5f);
};

} // namespace vv::vulkan
