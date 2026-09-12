#include "vulkan/VulkanRenderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Incremental region streaming: per-frame generation budget (ms) and the
// chunk-upload batch size. At ~0.8 ms/chunk these keep a border crossing
// (2(2r+1)-1 = 49 chunks at r=12) spread over ~13 frames instead of one
// ~50 ms hitch, while the old region (and the far field beyond it) keeps
// rendering.
//
// Pass 6: capped to ONE chunk per frame (VV_STREAM_CHUNKS overrides, for
// tuning). Each flushed batch costs a device idle + staging upload, and
// even one per frame was measurable as a micro-stutter at high fps - with
// the cap at 1 a border crossing takes ~49 frames (~0.8 s at 60 fps) to
// fully swap, invisible because the old region keeps rendering meanwhile.
constexpr double kStreamBudgetMs = 3.0;
constexpr std::size_t kStreamChunksPerFrame = 1;

// Fog tail attenuation used ONLY inside the shader's fog curve
// (fog = 1 - exp(-kFogTail * (d/cut)^4)); the C++ side never needs the value,
// it only supplies fogDensity = 1 / fogCutDistance(). See fogCutDistance().
// The exponent-4 curve keeps mid-distance terrain clear while forcing 99.8%
// opacity exactly at the cut, which hides the region boundary.

// Optional VK_EXT_debug_utils callback: routes validation-layer / driver
// messages to stderr. Harmless (and silent) when no layers are active.
VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
  }
  return VK_FALSE;
}

bool extensionSupported(const char* name,
                        const std::vector<VkExtensionProperties>& available) {
  return std::any_of(available.begin(), available.end(),
                     [name](const VkExtensionProperties& props) {
                       return std::strcmp(props.extensionName, name) == 0;
                     });
}

}  // namespace

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

  // Debug visualization (see docs/AGENT_NOTES.md): VV_DEBUG_TERM false-
  // colors each pixel by ray-termination cause.
  m_debugTerminators = std::getenv("VV_DEBUG_TERM") != nullptr;
  if (m_debugTerminators) {
    std::fprintf(stderr, "[vulkan] VV_DEBUG_TERM: on (miss pixels colored by "
                         "termination cause; see AGENT_NOTES)\n");
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
  if (m_deviceLost) {
    return;
  }

  vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE,
                  UINT64_MAX);

  // Fog density follows the camera: the cut distance is the current distance
  // to the nearest region side face (see fogCutDistance()). The shader cuts
  // rays at 1/fogDensity and is 99.8% opaque there.
  m_fogDensity = 1.0f / fogCutDistance();

  // Delegated to SceneUniform utility: updates camera + lighting UBO.
  m_sceneUniform.update(m_camera, m_timeSeconds, m_lighting,
                         glm::vec2(m_debugTerminators ? 1.0f : 0.0f, 0.0f));

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
    setDeviceLost("vkAcquireNextImageKHR failed (" +
                  utils::vkResultToString(acquire) + ").");
    return;
  }

  vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

  vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
  std::string recordError;
  if (!recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex,
                           recordError)) {
    setDeviceLost("Failed to record the frame command buffer: " + recordError);
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
    setDeviceLost("vkQueueSubmit failed.");
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
  } else if (present != VK_SUCCESS) {
    setDeviceLost("vkQueuePresentKHR failed (" +
                  utils::vkResultToString(present) + ").");
    return;
  }

  m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
  ++m_frameCounter;
}

void VulkanRenderer::setDeviceLost(const std::string& message) {
  if (m_deviceLost) {
    return;
  }
  m_deviceLost = true;
  m_lastError = message;
  std::fprintf(stderr, "[vulkan] fatal: %s\n", message.c_str());
}

void VulkanRenderer::cleanup() {
  // Join the far-LOD builder thread FIRST: it reads the terrain generator
  // owned by m_world (destroyed below) and fills m_farPending.
  if (m_farThread.joinable()) {
    m_farThread.join();
  }
  m_farBuildRunning = false;
  m_farPendingReady = false;

  if (m_device) {
    vkDeviceWaitIdle(m_device);
  }

  if (m_debugMessenger) {
    auto* destroyMessenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance,
                                  "vkDestroyDebugUtilsMessengerEXT"));
    if (destroyMessenger != nullptr) {
      destroyMessenger(m_instance, m_debugMessenger, nullptr);
    }
    m_debugMessenger = VK_NULL_HANDLE;
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

void VulkanRenderer::setWorldConfig(const vv::voxel::VoxelConfig& config) {
  if (m_initialized) {
    return;
  }

  if (!config.isValid()) {
    return;
  }

  m_voxelConfig = config;
  m_voxelConfig.renderRadiusChunks =
      std::min(m_voxelConfig.renderRadiusChunks, 16u);
  m_voxelConfig.maxTraceSteps =
      std::min(m_voxelConfig.maxTraceSteps, 4096u);
}

void VulkanRenderer::updateWorld(const glm::vec3& cameraPosition) {
  if (!m_initialized || !m_world) {
    return;
  }

  const auto& cfg = m_voxelConfig;
  const float chunkWorldX = static_cast<float>(cfg.chunkSizeX) * cfg.voxelSize.x;
  const float chunkWorldZ = static_cast<float>(cfg.chunkSizeZ) * cfg.voxelSize.z;
  const int32_t chunkX = static_cast<int32_t>(
      std::floor(cameraPosition.x / chunkWorldX));
  const int32_t chunkZ = static_cast<int32_t>(
      std::floor(cameraPosition.z / chunkWorldZ));

  if (m_streamActive) {
    if (chunkX != m_streamTarget.x || chunkZ != m_streamTarget.z) {
      // The target moved again mid-stream: re-aim (coords already streamed
      // stay in their slots; pending is recomputed against the new target).
      m_streamTarget = vv::voxel::ChunkCoord{chunkX, chunkZ};
      rebuildStreamPending();
      const std::size_t needed = m_streamPending.size();
      if (needed > m_freeSlots.size()) {
        // Too far for the spare ring (teleport-scale): fall back to the
        // synchronous rebuild.
        m_streamActive = false;
        m_streamPending.clear();
        std::string error;
        if (!rebuildChunkRegion(chunkX, chunkZ, error)) {
          std::fprintf(stderr, "[vulkan] chunk region update failed: %s\n",
                       error.c_str());
        }
      } else if (m_streamPending.empty()) {
        finishRegionMove();
      }
    } else if (std::abs(chunkX - m_regionCenter.x) > 2 ||
               std::abs(chunkZ - m_regionCenter.z) > 2) {
      // Streaming cannot keep up with a fast camera: catch up
      // synchronously (pass-6 behavior, restored by the pass-9 revert).
      m_streamActive = false;
      m_streamPending.clear();
      std::string error;
      if (!rebuildChunkRegion(chunkX, chunkZ, error)) {
        std::fprintf(stderr, "[vulkan] chunk region update failed: %s\n",
                     error.c_str());
      }
    } else {
      pumpRegionStreaming(kStreamBudgetMs);
    }
    ensureFarField(chunkX, chunkZ);
    return;
  }

  if (chunkX == m_regionCenter.x && chunkZ == m_regionCenter.z) {
    ensureFarField(chunkX, chunkZ);
    return;
  }

  beginRegionMove(chunkX, chunkZ);
  ensureFarField(chunkX, chunkZ);
}

void VulkanRenderer::beginRegionMove(int32_t targetChunkX,
                                     int32_t targetChunkZ) {
  m_streamTarget = vv::voxel::ChunkCoord{targetChunkX, targetChunkZ};
  rebuildStreamPending();

  if (m_streamPending.empty()) {
    // Everything already resident in slots: just swap.
    finishRegionMove();
    return;
  }
  if (m_streamPending.size() > m_freeSlots.size()) {
    // Not enough spare slots (teleport-scale move): synchronous rebuild.
    m_streamPending.clear();
    std::string error;
    if (!rebuildChunkRegion(targetChunkX, targetChunkZ, error)) {
      std::fprintf(stderr, "[vulkan] chunk region update failed: %s\n",
                   error.c_str());
    }
    return;
  }
  m_streamActive = true;
}

// Priority: chunks in front of the camera (frustum) first, then near ones.
// Sorted ascending (worst first) so pop_back() serves the best chunk.
static float streamPriority(const vv::voxel::ChunkCoord& coord,
                            const glm::vec3& cameraPos,
                            const glm::vec3& cameraForward, uint32_t chunkSize,
                            float voxelSize) {
  const glm::vec3 center(
      (static_cast<float>(coord.x) + 0.5f) * static_cast<float>(chunkSize) *
          voxelSize,
      0.0f,
      (static_cast<float>(coord.z) + 0.5f) * static_cast<float>(chunkSize) *
          voxelSize);
  glm::vec3 dir = center - cameraPos;
  dir.y = 0.0f;
  const float dist = std::max(glm::length(dir), 1.0f);
  const glm::vec3 f(cameraForward.x, 0.0f, cameraForward.z);
  const float facing =
      glm::length(f) > 1e-6f ? glm::dot(dir / dist, glm::normalize(f)) : 0.0f;
  return facing - dist * 0.0005f;
}

void VulkanRenderer::rebuildStreamPending() {
  const auto& cfg = m_voxelConfig;
  const int32_t r = static_cast<int32_t>(cfg.renderRadiusChunks);
  m_streamPending.clear();
  for (int32_t dz = -r; dz <= r; ++dz) {
    for (int32_t dx = -r; dx <= r; ++dx) {
      const vv::voxel::ChunkCoord coord{m_streamTarget.x + dx,
                                        m_streamTarget.z + dz};
      if (m_slotOf.find(coord) == m_slotOf.end()) {
        m_streamPending.push_back(coord);
      }
    }
  }
  const glm::vec3 pos = m_camera.position();
  const glm::vec3 fwd = m_camera.forward();
  std::sort(m_streamPending.begin(), m_streamPending.end(),
            [this, &pos, &fwd](const vv::voxel::ChunkCoord& a,
                               const vv::voxel::ChunkCoord& b) {
              return streamPriority(a, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x) <
                     streamPriority(b, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x);
            });
}

void VulkanRenderer::pumpRegionStreaming(double budgetMs) {
  if (!m_streamActive) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();

  // One chunk per frame (kStreamChunksPerFrame) through the FENCE-SCOPED
  // upload path: no vkDeviceWaitIdle / vkQueueWaitIdle / per-frame staging
  // allocation (those were the streaming stutter). The upload only touches
  // spare-ring slots no uploaded table references; finishRegionMove drains
  // everything with its own device wait before the table swap. The
  // remaining per-frame cost is the chunk generation itself (~2.8 ms).
  std::size_t streamed = 0;
  while (!m_streamPending.empty() && streamed < kStreamChunksPerFrame) {
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start;
    if (elapsed.count() * 1000.0 >= budgetMs) {
      break;
    }
    const vv::voxel::ChunkCoord coord = m_streamPending.back();
    m_streamPending.pop_back();
    const vv::voxel::Chunk* chunk = m_world->ensureChunk(coord);
    if (m_freeSlots.empty()) {
      m_streamPending.push_back(coord);
      break;
    }
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_slotOf[coord] = slot;
    std::string error;
    if (!m_voxelResources.uploadChunksStreaming(
            m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
            {slot, chunk}, error)) {
      std::fprintf(stderr, "[vulkan] streaming upload failed: %s\n",
                   error.c_str());
      m_slotOf.erase(coord);
      m_freeSlots.push_back(slot);
      m_streamPending.push_back(coord);
      break;
    }
    ++streamed;
  }
  if (m_streamPending.empty()) {
    finishRegionMove();
  }
}

void VulkanRenderer::finishRegionMove() {
  const auto& cfg = m_voxelConfig;
  const int32_t r = static_cast<int32_t>(cfg.renderRadiusChunks);

  // Release slots of chunks that left the new region (trailing edge and any
  // leftovers from abandoned stream targets).
  for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
    if (std::abs(it->first.x - m_streamTarget.x) > r ||
        std::abs(it->first.z - m_streamTarget.z) > r) {
      m_freeSlots.push_back(it->second);
      it = m_slotOf.erase(it);
    } else {
      ++it;
    }
  }

  // Evict the CPU cache beyond the usual +1 hysteresis ring.
  std::vector<vv::voxel::ChunkCoord> evicted;
  m_world->evictOutside(m_streamTarget.x, m_streamTarget.z,
                        cfg.renderRadiusChunks + 1, evicted);

  // Build and swap the region table. The device wait guarantees no frame
  // submitted since the last streaming upload still reads the old table -
  // AND that the final fence-scoped streaming upload has fully landed
  // before the table that references its slot goes live.
  std::vector<uint32_t> table(
      static_cast<std::size_t>(cfg.gridWidth()) * cfg.gridHeight(),
      vv::vulkan::VoxelResources::kEmptySlot);
  for (int32_t dz = -r; dz <= r; ++dz) {
    for (int32_t dx = -r; dx <= r; ++dx) {
      const vv::voxel::ChunkCoord coord{m_streamTarget.x + dx,
                                        m_streamTarget.z + dz};
      const auto it = m_slotOf.find(coord);
      if (it == m_slotOf.end()) {
        continue;  // cannot happen: streaming completes before finish
      }
      const std::size_t cell =
          static_cast<std::size_t>(dx + r) +
          static_cast<std::size_t>(dz + r) * cfg.gridWidth();
      table[cell] = it->second;
    }
  }
  vkDeviceWaitIdle(m_device);
  if (!m_voxelResources.writeChunkTable(table)) {
    std::fprintf(stderr, "[vulkan] region table swap failed\n");
  }
  m_regionCenter = m_streamTarget;
  m_streamActive = false;
  m_streamPending.clear();

  // The region boundary moved: re-derive the far field's seam band from
  // the now-active chunks so the far surface continues the exact terrain.
  if (patchFarFieldWithRegion()) {
    std::string error;
    if (!m_voxelResources.uploadFarField(m_device, m_physicalDevice,
                                         m_commandPool, m_graphicsQueue,
                                         m_farCells, error)) {
      std::fprintf(stderr, "[vulkan] far LOD seam patch upload failed: %s\n",
                   error.c_str());
    }
  }
}

void VulkanRenderer::launchFarFieldBuild(int32_t centerChunkX,
                                         int32_t centerChunkZ) {
  if (m_farBuildRunning.load() || !m_world ||
      m_voxelConfig.farLodRadiusChunks == 0) {
    return;
  }
  m_farPendingReady = false;
  m_farBuildRunning = true;
  m_farPendingCenterX = centerChunkX;
  m_farPendingCenterZ = centerChunkZ;

  // The thread only reads the const generator (owned by m_world, alive
  // until cleanup() joins this thread) and writes m_farPending, which the
  // main thread touches only after m_farPendingReady flips.
  const vv::terrain::TerrainGenerator* gen = &m_world->terrain();
  const uint32_t radius = m_voxelConfig.farLodRadiusChunks;
  const uint32_t cell = m_voxelConfig.farLodCellVoxels;
  const uint32_t chunkSize = m_voxelConfig.chunkSizeX;
  m_farThread = std::thread(
      [gen, centerChunkX, centerChunkZ, radius, cell, chunkSize, this]() {
        m_farPending = vv::terrain::FarField::build(
            *gen, centerChunkX, centerChunkZ, radius, cell, chunkSize);
        m_farPendingReady.store(true, std::memory_order_release);
      });
}

void VulkanRenderer::ensureFarField(int32_t centerChunkX,
                                    int32_t centerChunkZ) {
  if (m_voxelConfig.farLodRadiusChunks == 0 || !m_world) {
    return;
  }

  // A finished build is waiting: join, upload, activate.
  if (m_farBuildRunning.load() && m_farPendingReady.load()) {
    if (m_farThread.joinable()) {
      m_farThread.join();
    }
    m_farBuildRunning = false;

    const auto& cfg = m_voxelConfig;
    if (m_farPending.dim != 0 &&
        m_farPending.dim == cfg.farLodDim() &&
        m_farPending.cellVoxels == cfg.farLodCellVoxels) {
      m_farCells = m_farPending.cells;
      m_farOriginVoxX = m_farPending.originVoxX;
      m_farOriginVoxZ = m_farPending.originVoxZ;
      m_farDim = m_farPending.dim;
      m_farCell = m_farPending.cellVoxels;
      m_farFieldActive = true;
      // Recenter hysteresis is measured against the SNAPPED field center
      // (the build snaps to a world-aligned grid; see FarField::build).
      const std::int32_t chunkX32 =
          static_cast<std::int32_t>(m_voxelConfig.chunkSizeX);
      const auto voxToChunk = [&](std::int32_t v) {
        return (v >= 0 ? v : v - chunkX32 + 1) / chunkX32;
      };
      m_farCenterChunkX = voxToChunk(m_farPending.centerVoxX);
      m_farCenterChunkZ = voxToChunk(m_farPending.centerVoxZ);
      // The freshly estimated field would show its coarse seams exactly
      // where the near region ends - rewrite that band from the real
      // chunk data before the (always-required) first upload.
      patchFarFieldWithRegion();
      std::string uploadError;
      if (!m_voxelResources.uploadFarField(m_device, m_physicalDevice,
                                           m_commandPool, m_graphicsQueue,
                                           m_farCells, uploadError)) {
        std::fprintf(stderr, "[vulkan] far LOD upload failed: %s\n",
                     uploadError.c_str());
        m_farFieldActive = false;
      }
      std::fprintf(stderr,
                   "[vulkan] far LOD field active: %ux%u cells of %u voxels "
                   "at (%d,%d) (seam band patched from %zu chunks)\n",
                   m_farDim, m_farDim, m_farCell, m_farOriginVoxX,
                   m_farOriginVoxZ, m_slotOf.size());
    } else {
      std::fprintf(stderr,
                   "[vulkan] far LOD build rejected (dim %u vs %u, cell %u vs "
                   "%u)\n",
                   m_farPending.dim, cfg.farLodDim(), m_farPending.cellVoxels,
                   cfg.farLodCellVoxels);
    }
    m_farPendingReady = false;
    return;
  }

  // Recenter when the camera strays too far from the active field's center
  // (quarter of the radius, >= 4 chunks). The old field keeps rendering
  // until the new one is uploaded.
  if (!m_farBuildRunning.load()) {
    const int32_t threshold = std::max<int32_t>(
        4, static_cast<int32_t>(m_voxelConfig.farLodRadiusChunks) / 4);
    const int32_t dx = centerChunkX - m_farCenterChunkX;
    const int32_t dz = centerChunkZ - m_farCenterChunkZ;
    const bool needsRebuild = !m_farFieldActive ||
                              std::abs(dx) > threshold ||
                              std::abs(dz) > threshold;
    if (needsRebuild) {
      launchFarFieldBuild(centerChunkX, centerChunkZ);
    }
  }
}

bool VulkanRenderer::patchFarFieldWithRegion() {
  if (!m_farFieldActive || m_farDim == 0 || !m_world || m_farCells.empty()) {
    return false;
  }
  const auto& cfg = m_voxelConfig;
  std::vector<vv::terrain::FarField::RegionChunkHeights> chunks;
  chunks.reserve(m_slotOf.size());
  for (const auto& [coord, slot] : m_slotOf) {
    (void)slot;
    const vv::voxel::Chunk* chunk = m_world->findChunk(coord);
    if (chunk == nullptr) {
      continue;
    }
    chunks.push_back({coord.x * static_cast<std::int32_t>(cfg.chunkSizeX),
                      coord.z * static_cast<std::int32_t>(cfg.chunkSizeZ),
                      cfg.chunkSizeX, cfg.chunkSizeZ,
                      chunk->heightMap().data()});
  }
  return vv::terrain::FarField::patchRegion(
             m_farCells, m_farDim, m_farCell, m_farOriginVoxX, m_farOriginVoxZ,
             chunks, m_world->terrain()) > 0;
}

glm::vec3 VulkanRenderer::spawnPosition() const {
  const auto& cfg = m_voxelConfig;
  const float x = (static_cast<float>(cfg.chunkSizeX) * 0.5f) * cfg.voxelSize.x;
  const float z = (static_cast<float>(cfg.chunkSizeZ) * 0.5f) * cfg.voxelSize.z;
  float height = 64.0f;
  if (m_world) {
    // Topmost SOLID voxel (the density terrain can sit up to a mountain +
    // overhang-warp above the 2D heightAt, which used to be enough).
    const std::int32_t top = m_world->terrain().topSolidVoxels(
        static_cast<std::int32_t>(x), static_cast<std::int32_t>(z));
    height = static_cast<float>(top >= 0 ? top : 0);
  }
  return glm::vec3(x, height + 12.0f, z);
}

std::string VulkanRenderer::debugStats() const {
  const auto& cfg = m_voxelConfig;
  const float cut = 1.0f / std::max(m_fogDensity, 1e-9f);
  char buf[288];
  std::snprintf(buf, sizeof(buf),
                "fogCut=%.0fu fog=%.5f steps=%u skyCeil=%d far=%s(%ux%u@%d,%d)"
                " region=%ux%u@(%d,%d) slots=%zu/%llu",
                cut, m_fogDensity, cfg.maxTraceSteps, m_maxTerrainVoxelY,
                m_farFieldActive ? "on" : (m_farBuildRunning.load() ? "building" : "off"),
                m_farDim, m_farDim, m_farCenterChunkX, m_farCenterChunkZ,
                cfg.gridWidth(), cfg.gridHeight(), m_regionCenter.x,
                m_regionCenter.z, m_slotOf.size(),
                static_cast<unsigned long long>(cfg.slotCount()));
  return std::string(buf);
}

float VulkanRenderer::fogCutDistance() const {
  // The fog cut must make the boundary of whatever the ray can possibly
  // reach invisible. With the far-LOD field active that boundary is the FAR
  // box (the near-region boundary is hidden geometrically: the far field
  // continues the same terrain beyond it); without it, the near-region box
  // as before. Cut = distance from the camera to the nearest side face; any
  // ray's box exit lies at >= that perpendicular distance, i.e. in >= 99.8%
  // fog.
  const auto& cfg = m_voxelConfig;
  const float chunkWorldX =
      static_cast<float>(cfg.chunkSizeX) * cfg.voxelSize.x;
  const float chunkWorldZ =
      static_cast<float>(cfg.chunkSizeZ) * cfg.voxelSize.z;

  float x0, x1, z0, z1;
  if (m_farFieldActive) {
    x0 = static_cast<float>(m_farOriginVoxX) * cfg.voxelSize.x;
    x1 = x0 + static_cast<float>(m_farDim) * static_cast<float>(m_farCell) *
                  cfg.voxelSize.x;
    z0 = static_cast<float>(m_farOriginVoxZ) * cfg.voxelSize.z;
    z1 = z0 + static_cast<float>(m_farDim) * static_cast<float>(m_farCell) *
                  cfg.voxelSize.z;
  } else {
    const int32_t originX =
        m_regionCenter.x - static_cast<int32_t>(cfg.renderRadiusChunks);
    const int32_t originZ =
        m_regionCenter.z - static_cast<int32_t>(cfg.renderRadiusChunks);
    x0 = static_cast<float>(originX) * chunkWorldX;
    x1 = x0 + static_cast<float>(cfg.gridWidth()) * chunkWorldX;
    z0 = static_cast<float>(originZ) * chunkWorldZ;
    z1 = z0 + static_cast<float>(cfg.gridHeight()) * chunkWorldZ;
  }

  const glm::vec3 pos = m_camera.position();
  const float dx = std::min(pos.x - x0, x1 - pos.x);
  const float dz = std::min(pos.z - z0, z1 - pos.z);
  // Floor of one chunk extent guards the (transient) case of the camera
  // being outside the box after a failed region rebuild.
  const float floorDist = std::min(chunkWorldX, chunkWorldZ);
  return std::clamp(std::min(dx, dz), floorDist, 1e9f);
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
  std::vector<const char*> extensions =
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

  // Optional diagnostics: a debug messenger (validation-layer messages on
  // stderr when layers are present). The Khronos validation layer is only
  // enabled when the VV_VALIDATION environment variable is set, since it
  // carries a noticeable performance cost.
  std::vector<const char*> layers;
  if (extensionSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, available)) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
  if (std::getenv("VV_VALIDATION") != nullptr) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    if (layerCount > 0) {
      vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
    }
    const bool hasValidation = std::any_of(
        availableLayers.begin(), availableLayers.end(),
        [](const VkLayerProperties& props) {
          return std::strcmp(props.layerName,
                             "VK_LAYER_KHRONOS_validation") == 0;
        });
    if (hasValidation) {
      static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
      layers.push_back(kValidationLayer);
    } else {
      std::fprintf(stderr,
                   "[vulkan] VV_VALIDATION is set but the Khronos validation "
                   "layer is not installed; continuing without it.\n");
    }
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
  createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  VkResult r = vkCreateInstance(&createInfo, nullptr, &m_instance);
  if (r != VK_SUCCESS) {
    outError = "Failed to create Vulkan instance (" +
               utils::vkResultToString(r) +
               "). Vulkan might be missing or unsupported on this system.";
    return false;
  }

  // Install the debug messenger when the extension made it into the instance.
  if (std::find(extensions.begin(), extensions.end(),
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != extensions.end()) {
    auto* createMessenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (createMessenger != nullptr) {
      VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
      messengerInfo.sType =
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      messengerInfo.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      messengerInfo.messageType =
          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      messengerInfo.pfnUserCallback = debugUtilsCallback;
      (void)createMessenger(m_instance, &messengerInfo, nullptr,
                            &m_debugMessenger);
    }
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

  VkDescriptorSetLayoutBinding chunkTableBinding{};
  chunkTableBinding.binding = 3;
  chunkTableBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  chunkTableBinding.descriptorCount = 1;
  chunkTableBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding paletteBinding{};
  paletteBinding.binding = 4;
  paletteBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paletteBinding.descriptorCount = 1;
  paletteBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding heightBinding{};
  heightBinding.binding = 5;
  heightBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  heightBinding.descriptorCount = 1;
  heightBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding farBinding{};
  farBinding.binding = 6;
  farBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  farBinding.descriptorCount = 1;
  farBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

VkDescriptorSetLayoutBinding bindings[] = {
      voxelBufferBinding, outputBufferBinding, sceneBinding, chunkTableBinding,
      paletteBinding, heightBinding, farBinding};

  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = 7;
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
  if (!m_voxelConfig.isValid()) {
    outError = "Invalid voxel world configuration.";
    return false;
  }

  vv::terrain::TerrainConfig terrainConfig;
  terrainConfig.seed = m_voxelConfig.terrainSeed;
  m_world = std::make_unique<vv::voxel::World>(
      terrainConfig, m_voxelConfig.chunkSizeX, m_voxelConfig.worldHeight,
      m_voxelConfig.chunkSizeZ);

  // Sky-skip ceiling: a TRUE upper bound on solid terrain (voxel-space y).
  // The shader early-outs rays whose whole region segment stays above it,
  // so a value that is too low clips real terrain into noisy contour rings
  // (the 2.5 bug: this assignment was missing entirely and the ceiling
  // silently stayed 0, i.e. every ray that did not dive below the bedrock
  // floor was skipped). maxHeightVoxels() now bounds every SOLID voxel of
  // the density terrain (rolling base + mountain lift + overhang warp
  // band); clamp to the world top for safety.
  m_maxTerrainVoxelY = std::clamp(
      m_world->terrain().maxHeightVoxels(), std::int32_t{1},
      static_cast<std::int32_t>(m_voxelConfig.worldHeight - 1));

  if (!m_voxelResources.create(m_device, m_physicalDevice, m_voxelConfig,
                               outError)) {
    return false;
  }

  m_slotOf.clear();
  m_freeSlots.clear();
  m_freeSlots.reserve(m_voxelResources.slotCount());
  for (uint32_t slot = m_voxelResources.slotCount(); slot-- > 0;) {
    m_freeSlots.push_back(slot);
  }

  // Initial region around chunk (0,0); the camera spawns inside it.
  if (!rebuildChunkRegion(0, 0, outError)) {
    return false;
  }

  // Kick off the first far-LOD build on the background thread (~1M noise
  // evaluations at the default radius; renders start immediately with the
  // fog wall at the near-region boundary until the field pops in).
  launchFarFieldBuild(0, 0);

  // Safety net above the fog cut: the budget must never bind before the fog
  // does. Worst case a ray crosses ~sqrt(3) cells per unit of distance; the
  // fog cut is at most the region diagonal, so 1.75x the region width (in
  // voxels) covers every in-region ray with margin.
  const std::uint32_t regionWidthVoxels =
      m_voxelConfig.gridWidth() * m_voxelConfig.chunkSizeX;
  m_voxelConfig.maxTraceSteps =
      std::clamp<std::uint32_t>(m_voxelConfig.maxTraceSteps,
                                (regionWidthVoxels * 7u) / 4u, 4096u);

  m_fogDensity = 1.0f / fogCutDistance();
  return true;
}

bool VulkanRenderer::rebuildChunkRegion(int32_t centerChunkX,
                                        int32_t centerChunkZ,
                                        std::string& outError) {
  const uint32_t radius = m_voxelConfig.renderRadiusChunks;
  const int32_t r = static_cast<int32_t>(radius);
  const uint32_t gridW = m_voxelConfig.gridWidth();
  const uint32_t gridH = m_voxelConfig.gridHeight();
  const int32_t originX = centerChunkX - r;
  const int32_t originZ = centerChunkZ - r;

  std::vector<const vv::voxel::Chunk*> newChunks;
  std::vector<vv::voxel::ChunkCoord> evicted;
  m_world->ensureRegion(centerChunkX, centerChunkZ, radius, newChunks, evicted);

  for (const vv::voxel::ChunkCoord& coord : evicted) {
    const auto it = m_slotOf.find(coord);
    if (it != m_slotOf.end()) {
      m_freeSlots.push_back(it->second);
      m_slotOf.erase(it);
    }
  }

  // Every region cell needs an atlas slot: newly generated chunks AND chunks
  // that stayed CPU-cached but lost their slot earlier (they only need a
  // re-upload, not a regeneration).
  std::vector<std::pair<vv::voxel::ChunkCoord, const vv::voxel::Chunk*>>
      needUpload;
  for (int32_t gz = 0; gz < static_cast<int32_t>(gridH); ++gz) {
    for (int32_t gx = 0; gx < static_cast<int32_t>(gridW); ++gx) {
      const vv::voxel::ChunkCoord coord{originX + gx, originZ + gz};
      if (m_slotOf.find(coord) != m_slotOf.end()) {
        continue;
      }
      const vv::voxel::Chunk* chunk = m_world->findChunk(coord);
      if (chunk != nullptr) {
        needUpload.emplace_back(coord, chunk);
      }
    }
  }

  // Capacity fallback: with an exactly region-sized atlas, hysteresis
  // eviction lags one step behind demand, so the first crossing after any
  // stall would otherwise fail. Release slots of chunks outside the new
  // region (they stay CPU-cached and only need a re-upload later).
  if (needUpload.size() > m_freeSlots.size()) {
    for (auto it = m_slotOf.begin();
         it != m_slotOf.end() && needUpload.size() > m_freeSlots.size();) {
      if (std::abs(it->first.x - centerChunkX) > r ||
          std::abs(it->first.z - centerChunkZ) > r) {
        m_freeSlots.push_back(it->second);
        it = m_slotOf.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (needUpload.size() > m_freeSlots.size()) {
    outError = "Chunk atlas exhausted (need " +
               std::to_string(needUpload.size()) + " slots, have " +
               std::to_string(m_freeSlots.size()) + " free).";
    return false;
  }

  std::vector<vv::vulkan::VoxelResources::ChunkUpload> uploads;
  uploads.reserve(needUpload.size());
  for (const auto& [coord, chunk] : needUpload) {
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_slotOf[coord] = slot;
    uploads.push_back({slot, chunk});
  }

  if (!uploads.empty() &&
      !m_voxelResources.uploadChunks(m_device, m_physicalDevice, m_commandPool,
                                     m_graphicsQueue, uploads, outError)) {
    // Roll the slot bookkeeping back; chunks stay cached on the CPU side and
    // the next attempt re-uploads them into fresh slots.
    for (const vv::vulkan::VoxelResources::ChunkUpload& upload : uploads) {
      m_freeSlots.push_back(upload.slot);
      m_slotOf.erase(
          vv::voxel::ChunkCoord{upload.chunk->chunkX(), upload.chunk->chunkZ()});
    }
    return false;
  }

  m_regionCenter = vv::voxel::ChunkCoord{centerChunkX, centerChunkZ};

  // Rewrite the whole chunk table for the new region grid.
  std::vector<uint32_t> table(static_cast<size_t>(gridW) * gridH,
                              vv::vulkan::VoxelResources::kEmptySlot);
  for (const auto& [coord, slot] : m_slotOf) {
    const int32_t gx = coord.x - originX;
    const int32_t gz = coord.z - originZ;
    if (gx < 0 || gz < 0 || gx >= static_cast<int32_t>(gridW) ||
        gz >= static_cast<int32_t>(gridH)) {
      continue;
    }
    table[static_cast<size_t>(gx) + static_cast<size_t>(gz) * gridW] = slot;
  }
  if (!m_voxelResources.writeChunkTable(table)) {
    outError = "Failed to update the chunk table.";
    return false;
  }

  return true;
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
  VkDescriptorPoolSize poolSizes[2] = {};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 6;  // voxel atlas, output, chunk table,
                                     // palette, column heights, far LOD
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[1].descriptorCount = 1;

  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = 2;
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
  bufferInfo.buffer = m_voxelResources.voxelBuffer();
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

  VkDescriptorBufferInfo chunkTableInfo{};
  chunkTableInfo.buffer = m_voxelResources.chunkTableBuffer();
  chunkTableInfo.offset = 0;
  chunkTableInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo paletteInfo{};
  paletteInfo.buffer = m_voxelResources.paletteBuffer();
  paletteInfo.offset = 0;
  paletteInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo heightInfo{};
  heightInfo.buffer = m_voxelResources.heightBuffer();
  heightInfo.offset = 0;
  heightInfo.range = VK_WHOLE_SIZE;

  VkDescriptorBufferInfo farInfo{};
  farInfo.buffer = m_voxelResources.farBuffer();
  farInfo.offset = 0;
  farInfo.range = VK_WHOLE_SIZE;

  VkWriteDescriptorSet writes[7] = {};
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

  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = m_descriptorSet;
  writes[3].dstBinding = 3;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &chunkTableInfo;

  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = m_descriptorSet;
  writes[4].dstBinding = 4;
  writes[4].descriptorCount = 1;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[4].pBufferInfo = &paletteInfo;

  writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[5].dstSet = m_descriptorSet;
  writes[5].dstBinding = 5;
  writes[5].descriptorCount = 1;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[5].pBufferInfo = &heightInfo;

  writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[6].dstSet = m_descriptorSet;
  writes[6].dstBinding = 6;
  writes[6].descriptorCount = 1;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[6].pBufferInfo = &farInfo;

  vkUpdateDescriptorSets(m_device, 7, writes, 0, nullptr);
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
  preComputeBarriers[0].buffer = m_voxelResources.voxelBuffer();
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
  push.camera = glm::vec4(m_camera.tanHalfFovRadians(), m_fogDensity, 0.0f,
                          0.0f);
  push.chunkSize =
      glm::uvec4(m_voxelConfig.chunkSizeX, m_voxelConfig.worldHeight,
                 m_voxelConfig.chunkSizeZ, m_voxelConfig.maxTraceSteps);
  push.voxelSize = glm::vec4(m_voxelConfig.voxelSize, 0.0f);
  const int32_t originX =
      m_regionCenter.x - static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
  const int32_t originZ =
      m_regionCenter.z - static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
  push.region = glm::ivec4(originX, 0, originZ, 0);
  push.grid = glm::uvec4(m_voxelConfig.gridWidth(), m_voxelConfig.gridHeight(),
                         static_cast<uint32_t>(m_voxelResources.slotWordStride()),
                         static_cast<uint32_t>(m_maxTerrainVoxelY));
  if (m_farFieldActive) {
    push.far = glm::ivec4(m_farOriginVoxX, m_farOriginVoxZ,
                          static_cast<int32_t>(m_farDim),
                          static_cast<int32_t>(m_farDim));
  } else {
    push.far = glm::ivec4(0, 0, 0, 0);  // z = 0: far LOD off in the shader
  }
  if (m_farFieldActive) {
    push.farParams = glm::vec4(static_cast<float>(m_farCell), 0.0f, 0.0f,
                               0.0f);
  } else {
    push.farParams = glm::vec4(0.0f);
  }
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
