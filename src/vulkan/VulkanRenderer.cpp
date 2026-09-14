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
#include "render/VoxelTextureFiles.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "voxel/VoxelTypes.hpp"

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
constexpr std::size_t kStreamChunksPerFrame = 1;
// Sprint mode: when the camera outruns the base rate (more than 2 chunks
// ahead of the ACTIVE region center), the pump raises its per-frame cap and
// budget - the CPU is mostly idle while the GPU traces, so spending up to
// 10 ms/frame on generation is nearly free and drains the deficit in a few
// frames. This REPLACES the old >2-chunk synchronous catch-up, which
// generated everything at once (25 chunks per crossed row x 2.8 ms = 70 ms+
// hitches every couple of frames - the sprint stutter).
constexpr std::size_t kStreamSprintChunks = 16;
// How many chunks the generation worker may run ahead of the upload pump
// (bounds worker memory: N x 128 KB of staged voxel data).
constexpr std::size_t kGenBacklog = 6;
// Chunk fade-in duration (seconds) and the first far-field activation
// fade (recenters never fade - their cells are identical).
constexpr double kChunkFadeSeconds = 0.6;
constexpr double kFarFadeSeconds = 1.5;
// Sentinel for "VV_DEBUG_HOLE not set" (int32 max).
constexpr int32_t kHoleDebugOff = 0x7FFFFFFF;

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
  if (const char* perfEnv = std::getenv("VV_PERF")) {
    m_perfEnabled = std::strcmp(perfEnv, "0") != 0;
  }
  if (const char* holeEnv = std::getenv("VV_DEBUG_HOLE")) {
    int hx = 0, hz = 0;
    if (std::sscanf(holeEnv, "%d,%d", &hx, &hz) == 2) {
      m_holeDebugX = hx;
      m_holeDebugZ = hz;
      std::fprintf(stderr,
                   "[vulkan] VV_DEBUG_HOLE: far-miss pixels over chunk "
                   "(%d,%d) colored (magenta = empty far cell, cyan = data "
                   "present but ray passed over, yellow = ray never "
                   "crossed the chunk)\n",
                   hx, hz);
    }
  }
  if (m_debugTerminators) {
    std::fprintf(stderr, "[vulkan] VV_DEBUG_TERM: on (miss pixels colored by "
                         "termination cause; see AGENT_NOTES)\n");
  }
  if (std::getenv("VV_SHADOW_SHARP")) {
    m_shadowConeTan = 0.0f;
    std::fprintf(stderr,
                 "[vulkan] VV_SHADOW_SHARP: sun shadows use the exact "
                 "single-ray march (no cone)\n");
  }

  if (!createInstance(info, outError) || !createSurface(info, outError) ||
      !pickPhysicalDevice(outError) || !createDevice(outError) ||
      !createCommandPool(outError) || !createVoxelWorldAndUpload(outError) ||
      !createDescriptorSetLayout(outError) || !createSceneResources(outError)) {
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

  {
    const auto t0 = std::chrono::steady_clock::now();
    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE,
                    UINT64_MAX);
    m_perfGpuMs = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  }

  // Fog density follows the camera: the cut distance is the current distance
  // to the nearest region side face (see fogCutDistance()). The shader cuts
  // rays at 1/fogDensity and is 99.8% opaque there.
  m_fogDensity = 1.0f / fogCutDistance();

  // Delegated to SceneUniform utility: updates camera + lighting UBO.
  float farFade = 1.0f;
  if (m_farEverActivated) {
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() -
                               m_farFadeStart)
                               .count();
    farFade = static_cast<float>(
        std::min(elapsed / kFarFadeSeconds, 1.0));
  }
  m_sceneUniform.update(
      m_camera, m_timeSeconds, m_lighting,
      glm::vec4(m_debugTerminators ? 1.0f : 0.0f, farFade, m_shadowConeTan,
                0.0f));

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

  if (m_perfEnabled) {
    const auto now = std::chrono::steady_clock::now();
    const bool firstFrame =
        m_perfLastFrame.time_since_epoch().count() == 0;
    const double frameMs =
        firstFrame
            ? 0.0
            : std::chrono::duration<double, std::milli>(now - m_perfLastFrame)
                  .count();
    if (!firstFrame && frameMs > 25.0 &&
        std::chrono::duration<double, std::milli>(now - m_perfLastLog)
                .count() > 250.0) {
      std::fprintf(stderr,
                   "[perf] frame %u: %.1f ms | world %.1f (pump %.1f, "
                   "sync %.1f, far %.1f) | gpu-wait %.1f\n",
                   m_frameCounter, frameMs, m_perfWorldMs, m_perfPumpMs,
                   m_perfSyncMs, m_perfFarMs, m_perfGpuMs);
      m_perfLastLog = now;
    }
    m_perfLastFrame = now;
    m_perfWorldMs = 0.0;
    m_perfPumpMs = 0.0;
    m_perfSyncMs = 0.0;
    m_perfFarMs = 0.0;
    m_perfGpuMs = 0.0;
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
  // Stop the generation worker first (same reason as the far thread
  // below: it reads the terrain generator owned by m_world).
  stopGenerationWorker();
  // Join the far-LOD builder thread: it reads the terrain generator
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
  // Whole-body timing for VV_PERF (the pass-13 log only covered the pump,
  // and the actual hitch lived in an untimed bucket).
  struct ScopedTimer {
    VulkanRenderer& r;
    std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    ~ScopedTimer() {
      r.m_perfWorldMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    }
  } timer{*this};
  // Not streaming: discard any straggling worker results (the sync paths
  // generated their own data; keeping these would block a later stream's
  // completion check).
  if (!m_streamActive) {
    std::lock_guard<std::mutex> lock(m_genMutex);
    m_genResults.clear();
  }

  // Recycle released atlas slots once no in-flight frame can still
  // reference them (two frames after release; see finishRegionMove).
  for (auto it = m_slotCooldown.begin(); it != m_slotCooldown.end();) {
    if (m_frameCounter - it->second >= 2u) {
      m_freeSlots.push_back(it->first);
      it = m_slotCooldown.erase(it);
    } else {
      ++it;
    }
  }

  if (!m_initialized || !m_world) {
    return;
  }

  // Sliced far-LOD seam patch (a few chunks per frame; no-op cost is one
  // hash lookup per region cell when everything is patched).
  {
    const auto t0 = std::chrono::steady_clock::now();
    drainFarPatch();
    m_perfFarMs += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  }

  // Chunk fade-in alphas (binding 7): age every tracked slot, finalize
  // finished fades, publish the whole (tiny) array via mapped memory.
  if (!m_slotFadeStart.empty()) {
    const auto now = std::chrono::steady_clock::now();
    std::fill(m_slotFadeScratch.begin(), m_slotFadeScratch.end(), 1.0f);
    for (const auto& [coord, slot] : m_slotOf) {
      (void)coord;
      const auto& start = m_slotFadeStart[slot];
      if (start.time_since_epoch().count() == 0) {
        continue;  // opaque (not fading)
      }
      const double elapsed =
          std::chrono::duration<double>(now - start).count();
      const float alpha =
          static_cast<float>(std::min(elapsed / kChunkFadeSeconds, 1.0));
      if (alpha >= 1.0f) {
        m_slotFadeStart[slot] = {};
      } else {
        m_slotFadeScratch[slot] = alpha;
      }
    }
    m_voxelResources.writeChunkFade(m_slotFadeScratch);
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
        // Sprint overran the spare ring. The old response (synchronous
        // full rebuild) was the multi-second sprint freeze. Instead:
        // release the slots of chunks that left the new target's region
        // through the cooldown queue (in-flight frames may still read
        // them via the active table half - same rule as finishRegionMove)
        // and keep streaming. Only a genuine teleport (still short after
        // freeing) takes the synchronous path.
        const int32_t rr =
            static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
        for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
          if (std::abs(it->first.x - m_streamTarget.x) > rr ||
              std::abs(it->first.z - m_streamTarget.z) > rr) {
            m_slotCooldown.emplace_back(it->second, m_frameCounter);
            m_slotFadeStart[it->second] = {};
            it = m_slotOf.erase(it);
          } else {
            ++it;
          }
        }
        // A slot shortage is NOT an error state: the pump installs what
        // fits (its cap) and the cooldown recycles the freed slots two
        // frames later, so the deficit drains on its own. Only a genuine
        // teleport - more than half the region missing - still takes the
        // synchronous path (it beats tens of seconds of far-LOD-only
        // world). (Promoting just-freed cooldown entries immediately is
        // NOT safe: in-flight frames still reference them via the active
        // table half.)
        const std::size_t regionChunks = static_cast<std::size_t>(
            m_voxelConfig.gridWidth()) * m_voxelConfig.gridHeight();
        if (m_streamPending.size() > regionChunks / 2) {
          m_streamActive = false;
          m_streamPending.clear();
          std::string error;
          if (!rebuildChunkRegion(chunkX, chunkZ, error)) {
            std::fprintf(stderr,
                         "[vulkan] chunk region update failed: %s\n",
                         error.c_str());
          }
        }
      } else if (m_streamPending.empty()) {
        finishRegionMove();
      }
    }
    // No synchronous catch-up anymore: a fast camera just flips the pump
    // into sprint mode (see kStreamSprintChunks), which drains the deficit
    // within a few frames instead of hitching. Teleports still take the
    // fallback above.
    {
      const auto t0 = std::chrono::steady_clock::now();
      pumpRegionStreaming();
      m_perfPumpMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    }
    {
      const auto t0 = std::chrono::steady_clock::now();
      ensureFarField(chunkX, chunkZ);
      m_perfFarMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    }
    return;
  }

  if (chunkX == m_regionCenter.x && chunkZ == m_regionCenter.z) {
    const auto t0 = std::chrono::steady_clock::now();
    ensureFarField(chunkX, chunkZ);
    m_perfFarMs += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    return;
  }

  {
    const auto t0 = std::chrono::steady_clock::now();
    beginRegionMove(chunkX, chunkZ);
    m_perfPumpMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  }
  {
    const auto t0 = std::chrono::steady_clock::now();
    ensureFarField(chunkX, chunkZ);
    m_perfFarMs += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  }
}

void VulkanRenderer::beginRegionMove(int32_t targetChunkX,
                                     int32_t targetChunkZ) {
  // Drop queued generation requests for the old target; finished results
  // are filtered by the pump against the new target.
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    m_genRequests.clear();
  }
  if (m_genThreads.empty() && m_world) {
    m_genStop = false;
    m_genThreads.reserve(kGenWorkers);
    for (std::size_t k = 0; k < kGenWorkers; ++k) {
      m_genThreads.emplace_back(&VulkanRenderer::generationWorker, this,
                                &m_world->terrain());
    }
  }
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
  // NEAR-FIRST with a forward bias: priority = -(dist - facing * 96).
  // The old facing-dominant order generated the far edge ahead of the
  // camera before the chunks right next to it (at startup the NEAREST
  // chunks face away/sideways and came last - "takes a while for nearby
  // chunks"). Now near chunks always come first, ties/equal distance go
  // to what is in front (96 world units of bias ~ 3 chunks).
  return -(dist - facing * 96.0f);
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
  // Generate-only ring: the (r+1) hysteresis ring around the target, for
  // the far-LOD seam patch. Those chunks get NO atlas slot - they exist so
  // the seam band (region edge + one chunk beyond) is patched from REAL
  // column tops instead of the estimate, which under-shoots folded
  // mountain terrain by up to ~25 voxels (the "missing chunks at the
  // render-distance edge" holes).
  m_streamRingPending.clear();
  if (m_world != nullptr) {
    for (int32_t dz = -r - 1; dz <= r + 1; ++dz) {
      for (int32_t dx = -r - 1; dx <= r + 1; ++dx) {
        if (dx >= -r && dx <= r && dz >= -r && dz <= r) {
          continue;  // region cell: slot-bound above
        }
        const vv::voxel::ChunkCoord coord{m_streamTarget.x + dx,
                                          m_streamTarget.z + dz};
        if (m_world->findChunk(coord) == nullptr) {
          m_streamRingPending.push_back(coord);
        }
      }
    }
  }
  const glm::vec3 pos = m_camera.position();
  const glm::vec3 fwd = m_camera.forward();
  std::sort(m_streamRingPending.begin(), m_streamRingPending.end(),
            [this, &pos, &fwd](const vv::voxel::ChunkCoord& a,
                               const vv::voxel::ChunkCoord& b) {
              return streamPriority(a, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x) <
                     streamPriority(b, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x);
            });
  std::sort(m_streamPending.begin(), m_streamPending.end(),
            [this, &pos, &fwd](const vv::voxel::ChunkCoord& a,
                               const vv::voxel::ChunkCoord& b) {
              return streamPriority(a, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x) <
                     streamPriority(b, pos, fwd, m_voxelConfig.chunkSizeX,
                                    m_voxelConfig.voxelSize.x);
            });
}

void VulkanRenderer::generationWorker(
    const vv::terrain::TerrainGenerator* gen) {
  const auto& cfg = m_voxelConfig;
  for (;;) {
    vv::voxel::ChunkCoord coord{};
    {
      std::unique_lock<std::mutex> lock(m_genMutex);
      m_genCV.wait(lock, [this] { return m_genStop || !m_genRequests.empty(); });
      if (m_genRequests.empty()) {
        return;  // stop requested and nothing left
      }
      coord = m_genRequests.back();
      m_genRequests.pop_back();
    }
    // generateChunkVoxels is const and thread-safe (same contract as the
    // far-LOD build thread); ~2.8 ms per chunk.
    std::vector<std::uint8_t> types;
    gen->generateChunkVoxels(
        coord.x * static_cast<std::int32_t>(cfg.chunkSizeX),
        coord.z * static_cast<std::int32_t>(cfg.chunkSizeZ), cfg.chunkSizeX,
        cfg.chunkSizeZ, cfg.worldHeight, types);
    {
      std::lock_guard<std::mutex> lock(m_genMutex);
      m_genResults.push_back(GeneratedChunk{coord, std::move(types)});
    }
  }
}

void VulkanRenderer::stopGenerationWorker() {
  if (m_genThreads.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    m_genStop = true;
    m_genRequests.clear();
  }
  m_genCV.notify_all();
  for (std::thread& t : m_genThreads) {
    if (t.joinable()) {
      t.join();
    }
  }
  m_genThreads.clear();
  m_genResults.clear();
}

void VulkanRenderer::pumpRegionStreaming() {
  // 1) Collect finished generations (stale ones are discarded below).
  std::vector<GeneratedChunk> done;
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    done.swap(m_genResults);
  }

  // 2) Install + upload finished chunks that are still wanted. The upload
  //    path is fence-scoped (waits only the previous pump's submit), so
  //    this step is sub-millisecond per chunk.
  const int32_t r = static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
  const std::size_t deficit = static_cast<std::size_t>(std::max(
      std::abs(m_streamTarget.x - m_regionCenter.x),
      std::abs(m_streamTarget.z - m_regionCenter.z)));
  const std::size_t uploadCap =
      deficit > 2 ? kStreamSprintChunks : kStreamChunksPerFrame;
  std::size_t uploaded = 0;
  for (GeneratedChunk& gen : done) {
    if (!m_streamActive) {
      gen.types.clear();  // streaming aborted: discard (never re-queue)
      continue;
    }
    const bool wanted =
        m_slotOf.find(gen.coord) == m_slotOf.end() &&
        (std::find(m_streamPending.begin(), m_streamPending.end(),
                   gen.coord) != m_streamPending.end() ||
         std::find(m_streamRingPending.begin(), m_streamRingPending.end(),
                   gen.coord) != m_streamRingPending.end());
    const bool inRange =
        std::abs(gen.coord.x - m_streamTarget.x) <= r + 1 &&
        std::abs(gen.coord.z - m_streamTarget.z) <= r + 1;
    if (!wanted || !inRange) {
      gen.types.clear();  // not wanted: discard (never re-queue)
      continue;
    }
    // Ring chunks (generate-only, for the far seam patch): install into
    // the world, no slot, no upload, not capped (a move + map insert).
    if (std::find(m_streamRingPending.begin(), m_streamRingPending.end(),
                  gen.coord) != m_streamRingPending.end()) {
      m_world->installChunk(gen.coord, std::move(gen.types));
      m_streamRingPending.erase(std::remove(m_streamRingPending.begin(),
                                            m_streamRingPending.end(),
                                            gen.coord),
                                m_streamRingPending.end());
      continue;
    }
    if (uploaded >= uploadCap) {
      break;  // enough this frame; the rest is re-queued below
    }
    if (m_freeSlots.empty()) {
      continue;  // cannot happen mid-stream (pending fits the spare ring)
    }
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    const vv::voxel::Chunk* chunk =
        m_world->installChunk(gen.coord, std::move(gen.types));
    m_slotOf[gen.coord] = slot;
    m_slotFadeStart[slot] = std::chrono::steady_clock::now();
    std::string error;
    if (!m_voxelResources.uploadChunksStreaming(
            m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
            {slot, chunk}, error)) {
      std::fprintf(stderr, "[vulkan] streaming upload failed: %s\n",
                   error.c_str());
      m_slotOf.erase(gen.coord);
      m_freeSlots.push_back(slot);
      continue;
    }
    m_streamPending.erase(std::remove(m_streamPending.begin(),
                                      m_streamPending.end(), gen.coord),
                           m_streamPending.end());
    ++uploaded;
  }
  // Chunks become visible AS THEY INSTALL: without this the table only
  // published at completion and the whole region popped in at once
  // ("nearby chunks appear all at once"). The table covers the TARGET
  // grid; m_slotOf still holds old-region chunks, so the overlap stays
  // visible during crossings too.
  if (uploaded > 0) {
    publishRegionTable(false);
  }
  // Leftovers (not uploaded this frame) go back for the next pump.
  if (!done.empty()) {
    std::lock_guard<std::mutex> lock(m_genMutex);
    for (GeneratedChunk& gen : done) {
      if (!gen.types.empty()) {
        m_genResults.push_back(std::move(gen));
      }
    }
  }

  // 3) Top up the worker's queue from the pending set.
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    const auto tryQueue = [&](const vv::voxel::ChunkCoord& coord) {
      const bool queued =
          std::find(m_genRequests.begin(), m_genRequests.end(), coord) !=
              m_genRequests.end() ||
          std::any_of(m_genResults.begin(), m_genResults.end(),
                      [&](const GeneratedChunk& g) {
                        return g.coord == coord;
                      });
      if (!queued &&
          m_genRequests.size() + m_genResults.size() < kGenBacklog) {
        m_genRequests.push_back(coord);
      }
    };
    // Backlog filling order: both lists are sorted ascending (worst
    // first), and the worker pops from the BACK - so the backlog must be
    // filled in REVERSE (best first). Filling from the front (pass 17
    // bug) kept the backlog stocked with the WORST pending chunks: the
    // workers ground through the list farthest-first ("sorted
    // backwards"). With reverse filling the backlog always holds the
    // top-k best coords and generation runs nearest-ahead-first; the
    // ring (queued after the region in reverse order) still only starts
    // once the region's best are taken.
    for (auto it = m_streamPending.rbegin(); it != m_streamPending.rend();
         ++it) {
      tryQueue(*it);
    }
    for (auto it = m_streamRingPending.rbegin();
         it != m_streamRingPending.rend(); ++it) {
      tryQueue(*it);
    }
    m_genCV.notify_all();
  }

  // 4) Done when everything pending has been generated AND uploaded.
  bool finished = false;
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    finished = m_streamPending.empty() && m_streamRingPending.empty() &&
               m_genRequests.empty() && m_genResults.empty();
  }
  if (finished && m_streamActive) {
    finishRegionMove();
  }
}

// Writes the current slot map over the TARGET region grid into the next
// table half and flips the half index. Called incrementally by the pump
// (chunks appear as they install) and finally by finishRegionMove
// (logHoles = true: a completed region must have no empty cells).
// In-flight frames keep reading the half they were recorded with; three
// halves make consecutive-frame flips safe (the one being rewritten was
// last read by a frame whose fence the loop has since waited).
void VulkanRenderer::publishRegionTable(bool logHoles) {
  const auto& cfg = m_voxelConfig;
  const int32_t r = static_cast<int32_t>(cfg.renderRadiusChunks);
  const int32_t originX = m_streamTarget.x - r;
  const int32_t originZ = m_streamTarget.z - r;
  std::vector<uint32_t> table(
      static_cast<std::size_t>(cfg.gridWidth()) * cfg.gridHeight(),
      vv::vulkan::VoxelResources::kEmptySlot);
  // m_slotOf also holds chunks of the PREVIOUS region that overlap the
  // target grid - they stay visible during crossings until the swap.
  for (const auto& [coord, slot] : m_slotOf) {
    const int32_t gx = coord.x - originX;
    const int32_t gz = coord.z - originZ;
    if (gx < 0 || gz < 0 || gx >= static_cast<int32_t>(cfg.gridWidth()) ||
        gz >= static_cast<int32_t>(cfg.gridHeight())) {
      continue;
    }
    table[static_cast<std::size_t>(gx) +
          static_cast<std::size_t>(gz) * cfg.gridWidth()] = slot;
  }
  const uint32_t nextHalf =
      (m_tableHalf + 1u) % vv::vulkan::VoxelResources::kTableHalves;
  if (!m_voxelResources.writeChunkTable(table, nextHalf)) {
    std::fprintf(stderr, "[vulkan] region table publish failed\n");
    return;
  }
  m_tableHalf = nextHalf;
  m_tableOriginX = m_streamTarget.x;
  m_tableOriginZ = m_streamTarget.z;

  if (logHoles) {
    std::size_t holes = 0;
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (table[i] == vv::vulkan::VoxelResources::kEmptySlot) {
        ++holes;
        if (holes <= 5) {
          const int32_t gx =
              static_cast<int32_t>(i % cfg.gridWidth()) - r;
          const int32_t gz =
              static_cast<int32_t>(i / cfg.gridWidth()) - r;
          std::fprintf(stderr,
                       "[vulkan] TABLE HOLE at chunk (%d,%d) "
                       "(region %d,%d)\n",
                       m_streamTarget.x + gx, m_streamTarget.z + gz,
                       m_streamTarget.x, m_streamTarget.z);
        }
      }
    }
    if (holes > 5) {
      std::fprintf(stderr, "[vulkan] ... %zu more table holes\n",
                   holes - 5);
    }
  }
}

void VulkanRenderer::finishRegionMove() {
  const auto& cfg = m_voxelConfig;
  const int32_t r = static_cast<int32_t>(cfg.renderRadiusChunks);

  // The last fence-scoped streaming upload must land before a table that
  // references its slot goes live. Waiting the stream fence costs the
  // tail of one ~130 KB copy (sub-millisecond) - NOT a device drain.
  m_voxelResources.waitStreamingUploadIdle(m_device);

  // Release slots of chunks that left the new region (trailing edge and
  // any leftovers from abandoned stream targets). Into the COOLDOWN queue,
  // not the free list: in-flight frames still read the previous table
  // half, which references these slots. They are recycled two frames
  // later, once every frame that could reference them has been fence-
  // waited by the normal frame loop.
  for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
    if (std::abs(it->first.x - m_streamTarget.x) > r ||
        std::abs(it->first.z - m_streamTarget.z) > r) {
      m_slotCooldown.emplace_back(it->second, m_frameCounter);
      m_slotFadeStart[it->second] = {};  // back to opaque for lingering refs
      it = m_slotOf.erase(it);
    } else {
      ++it;
    }
  }

  // Evict the CPU cache beyond the usual +1 hysteresis ring.
  std::vector<vv::voxel::ChunkCoord> evicted;
  // Amortized (max 8 per swap): freeing a whole crossing row of 128 KB
  // chunk buffers in one call was visible allocator churn.
  m_world->evictOutside(m_streamTarget.x, m_streamTarget.z,
                        cfg.renderRadiusChunks + 1, evicted, 8);

  // Final publish (logs holes: after completion every region cell must
  // have a slot; an empty one is a real missing chunk).
  publishRegionTable(true);


  // The seam patch now drains incrementally from updateWorld
  // (drainFarPatch); nothing to do here.
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
  // Snapshot the active field for window reuse: the thread only READS
  // this copy while the main thread may keep patching m_farCells. With a
  // world-aligned grid a recenter only shifts the window, so the build
  // copies ~87% of the cells and recomputes just the exposed strips.
  bool havePrev = m_farFieldActive && m_farDim != 0;
  vv::terrain::FarField prev;
  if (havePrev) {
    prev.dim = m_farDim;
    prev.cellVoxels = m_farCell;
    prev.originVoxX = m_farOriginVoxX;
    prev.originVoxZ = m_farOriginVoxZ;
    prev.centerVoxX = m_farOriginVoxX +
                      static_cast<std::int32_t>(m_farDim * m_farCell / 2u);
    prev.centerVoxZ = m_farOriginVoxZ +
                      static_cast<std::int32_t>(m_farDim * m_farCell / 2u);
    prev.cells = m_farCells;  // ~4 MB copy, launch-time only
  }
  m_farThread = std::thread(
      [gen, centerChunkX, centerChunkZ, radius, cell, chunkSize, havePrev,
       prev = std::move(prev), this]() mutable {
        m_farPending = vv::terrain::FarField::build(
            *gen, centerChunkX, centerChunkZ, radius, cell, chunkSize,
            havePrev ? &prev : nullptr);
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
      if (!m_farEverActivated) {
        m_farEverActivated = true;
        m_farFadeStart = std::chrono::steady_clock::now();
      }
      // Recenter hysteresis is measured against the SNAPPED field center
      // (the build snaps to a world-aligned grid; see FarField::build).
      const std::int32_t chunkX32 =
          static_cast<std::int32_t>(m_voxelConfig.chunkSizeX);
      const auto voxToChunk = [&](std::int32_t v) {
        return (v >= 0 ? v : v - chunkX32 + 1) / chunkX32;
      };
      m_farCenterChunkX = voxToChunk(m_farPending.centerVoxX);
      m_farCenterChunkZ = voxToChunk(m_farPending.centerVoxZ);
      // The extent is NOT reset across (world-aligned) window shifts:
      // cells keep their world positions, so previously patched cells
      // stay exact through the window-reuse copy chain. Any newly exposed
      // seam band is patched incrementally by drainFarPatch over the next
      // frames - no synchronous full-region scan here (that was one of
      // the perf-log hitch spikes).
      // Upload the field into the INACTIVE half, then flip the
      // half index. No in-flight frame reads that half; the fence-scoped
      // upload waits only its own copy before the flip. No device wait.
      const uint32_t nextFarHalf =
          (m_farHalf + 1u) % vv::vulkan::VoxelResources::kFarHalves;
      std::string uploadError;
      if (!m_voxelResources.uploadFarFieldHalf(m_device, m_physicalDevice,
                                               m_commandPool, m_graphicsQueue,
                                               m_farCells, nextFarHalf,
                                               uploadError)) {
        std::fprintf(stderr, "[vulkan] far LOD upload failed: %s\n",
                     uploadError.c_str());
        m_farFieldActive = false;
      } else {
        m_farHalf = nextFarHalf;
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
    // Recenter rule with the world-aligned snap: the camera's SNAP CELL
    // (512-voxel grid) must match the field's, otherwise the window has
    // fallen behind. Comparing raw chunk distance to the SNAPPED center
    // never fired (the snap keeps the apparent distance <= ~8 chunks
    // forever - the field never moved after startup; found via the
    // owner's perf log showing exactly one activation).
    const std::int32_t chunkVox =
        static_cast<std::int32_t>(m_voxelConfig.chunkSizeX);
    const std::int32_t camVoxX = centerChunkX * chunkVox + chunkVox / 2;
    const std::int32_t camVoxZ = centerChunkZ * chunkVox + chunkVox / 2;
    const auto snapCell = [chunkVox](std::int32_t v) {
      const std::int32_t grid = 512;
      const std::int32_t rem = ((v % grid) + grid) % grid;
      return v - rem;
    };
    const bool needsRebuild =
        !m_farFieldActive ||
        snapCell(camVoxX) != snapCell(m_farCenterChunkX * chunkVox +
                                      chunkVox / 2) ||
        snapCell(camVoxZ) != snapCell(m_farCenterChunkZ * chunkVox +
                                      chunkVox / 2);
    if (needsRebuild) {
      launchFarFieldBuild(centerChunkX, centerChunkZ);
    }
  }
}

// Rewrites the far-LOD cells covered by the region's chunks with the REAL
// per-column tops. INCREMENTAL: only chunks outside the already-patched
// extent box are scanned (interior cells keep their exact values from
// earlier patches), which keeps the per-swap cost to the newly entered
// ring (~0.3 ms) instead of the whole region. Returns the changed cell
// runs (offset,count) + values for a delta upload.
// Sliced far-LOD seam patch: scans at most kFarPatchSlice cached chunks
// per call, patches their cells from REAL column tops, grows the patched
// extent over EXACTLY the scanned chunks, and pushes the changed cells as
// a small async delta upload. Called every updateWorld; a full-region
// patch (e.g. after the async startup, ~625 chunks) therefore spreads
// over ~40 frames instead of one 50-100 ms hitch (the pass-15 log's
// "far 96.8 ms" frame). The extent must only grow over chunks actually
// SCANNED: the old code marked the whole region box as patched even when
// the chunks did not exist yet (async startup), which would have left
// the seam on raw estimates - the edge holes - for good.
std::size_t VulkanRenderer::drainFarPatch() {
  if (!m_farFieldActive || m_farDim == 0 || !m_world || m_farCells.empty()) {
    return 0;
  }
  const auto& cfg = m_voxelConfig;
  const int32_t r = static_cast<int32_t>(cfg.renderRadiusChunks);
  constexpr std::size_t kFarPatchSlice = 16;

  // Cached chunks in the (r+1) square around the ACTIVE region center
  // that are outside the patched extent box, nearest-first (the box walk
  // starts at dz = -r-1; good enough). The +1 ring matters: the first
  // chunk row BEYOND the region edge is rendered by far cells, and
  // without real data there the estimate's under-shoot in folded columns
  // showed as chunk-shaped holes exactly at the render-distance edge.
  std::vector<vv::terrain::FarField::RegionChunkHeights> chunks;
  int32_t scanMinX = 0, scanMaxX = -1, scanMinZ = 0, scanMaxZ = -1;
  for (int32_t dz = -r - 1; dz <= r + 1 && chunks.size() < kFarPatchSlice;
       ++dz) {
    for (int32_t dx = -r - 1; dx <= r + 1 && chunks.size() < kFarPatchSlice;
         ++dx) {
      const int32_t cx = m_regionCenter.x + dx;
      const int32_t cz = m_regionCenter.z + dz;
      if (m_farPatchMaxX >= m_farPatchMinX && cx >= m_farPatchMinX &&
          cx <= m_farPatchMaxX && cz >= m_farPatchMinZ &&
          cz <= m_farPatchMaxZ) {
        continue;  // already exact from an earlier patch
      }
      const vv::voxel::Chunk* chunk =
          m_world->findChunk(vv::voxel::ChunkCoord{cx, cz});
      if (chunk == nullptr) {
        continue;  // not generated yet; a later frame picks it up
      }
      chunks.push_back({cx * static_cast<std::int32_t>(cfg.chunkSizeX),
                        cz * static_cast<std::int32_t>(cfg.chunkSizeZ),
                        cfg.chunkSizeX, cfg.chunkSizeZ,
                        chunk->heightMap().data()});
      scanMinX = (scanMaxX < scanMinX) ? cx : std::min(scanMinX, cx);
      scanMaxX = (scanMaxX < scanMinX) ? cx : std::max(scanMaxX, cx);
      scanMinZ = (scanMaxZ < scanMinZ) ? cz : std::min(scanMinZ, cz);
      scanMaxZ = (scanMaxZ < scanMinZ) ? cz : std::max(scanMaxZ, cz);
    }
  }
  if (chunks.empty()) {
    return 0;
  }

  std::vector<std::uint32_t> changed;
  vv::terrain::FarField::patchRegion(
      m_farCells, m_farDim, m_farCell, m_farOriginVoxX, m_farOriginVoxZ,
      chunks, m_world->terrain(), &changed);

  // Grow the extent over exactly the scanned chunks (NOT the target box).
  if (m_farPatchMaxX < m_farPatchMinX) {
    m_farPatchMinX = scanMinX;
    m_farPatchMaxX = scanMaxX;
    m_farPatchMinZ = scanMinZ;
    m_farPatchMaxZ = scanMaxZ;
  } else {
    m_farPatchMinX = std::min(m_farPatchMinX, scanMinX);
    m_farPatchMaxX = std::max(m_farPatchMaxX, scanMaxX);
    m_farPatchMinZ = std::min(m_farPatchMinZ, scanMinZ);
    m_farPatchMaxZ = std::max(m_farPatchMaxZ, scanMaxZ);
  }
  if (changed.empty()) {
    return chunks.size();
  }

  // Coalesce changed indices into contiguous runs for the delta upload.
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  std::vector<uint32_t> values;
  uint32_t runStart = changed[0];
  uint32_t runLen = 1;
  for (std::size_t i = 1; i <= changed.size(); ++i) {
    const bool flush = i == changed.size() || changed[i] != changed[i - 1] + 1;
    if (flush) {
      runs.emplace_back(runStart, runLen);
      for (uint32_t k = 0; k < runLen; ++k) {
        values.push_back(m_farCells[runStart + k]);
      }
      if (i < changed.size()) {
        runStart = changed[i];
        runLen = 1;
      }
    } else {
      ++runLen;
    }
  }
  std::string error;
  if (!m_voxelResources.uploadFarFieldDelta(
          m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
          m_farHalf, runs, values, error)) {
    std::fprintf(stderr, "[vulkan] far LOD seam patch upload failed: %s\n",
                 error.c_str());
  }
  return chunks.size();
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
  // Bindless voxel textures (binding 8): non-uniform indexing into a
  // sampled-image array. Requires the descriptor-indexing feature set
  // (core since Vulkan 1.2).
  features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
  VkPhysicalDeviceVulkan12Features v12Features{};
  v12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  v12Features.descriptorIndexing = VK_TRUE;
  v12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  {
    // Verify support before creating the device; a clear message beats a
    // device-lost later. vkGetPhysicalDeviceFeatures2 needs an instance
    // of 1.1+; without it descriptor indexing cannot exist anyway.
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    auto fpEnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (fpEnumerateInstanceVersion) {
      fpEnumerateInstanceVersion(&instanceVersion);
    }
    if (instanceVersion < VK_API_VERSION_1_2) {
      outError =
          "Vulkan 1.2 instance required (bindless voxel textures).";
      return false;
    }
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supported2);
    if (!supported12.descriptorIndexing ||
        !supported12.shaderSampledImageArrayNonUniformIndexing ||
        !supported2.features.shaderSampledImageArrayDynamicIndexing) {
      outError =
          "This GPU/driver lacks Vulkan 1.2 descriptor indexing "
          "(required for the bindless voxel texture array).";
      return false;
    }
  }

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
  createInfo.pQueueCreateInfos = queueInfos.data();
  createInfo.pEnabledFeatures = &features;
  createInfo.pNext = &v12Features;
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

  // Per-slot fade-in alphas (pass 19).
  VkDescriptorSetLayoutBinding fadeBinding{};
  fadeBinding.binding = 7;
  fadeBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  fadeBinding.descriptorCount = 1;
  fadeBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  // Bindless voxel textures (pass 20/21): one sampled image per texture
  // FILE, indexed non-uniformly per ray. The array is declared at
  // CAPACITY (kMaxVoxelTextures); the descriptor write binds the loaded
  // images and fills the rest with the white dummy view. Sampler at
  // binding 9; per-type face table (which image serves which face, or
  // the plain-color sentinel) at binding 10.
  VkDescriptorSetLayoutBinding textureArrayBinding{};
  textureArrayBinding.binding = 8;
  textureArrayBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  textureArrayBinding.descriptorCount = vv::voxel::kMaxVoxelTextures;
  textureArrayBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutBinding textureSamplerBinding{};
  textureSamplerBinding.binding = 9;
  textureSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  textureSamplerBinding.descriptorCount = 1;
  textureSamplerBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutBinding texInfoBinding{};
  texInfoBinding.binding = 10;
  texInfoBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  texInfoBinding.descriptorCount = 1;
  texInfoBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

VkDescriptorSetLayoutBinding bindings[] = {
      voxelBufferBinding, outputBufferBinding, sceneBinding, chunkTableBinding,
      paletteBinding, heightBinding, farBinding, fadeBinding,
      textureArrayBinding, textureSamplerBinding, texInfoBinding};

  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = 11;
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
  // Voxel textures from resources/textures/voxels (user's machine);
  // missing files fall back to plain palette colors (see
  // vv::render::loadVoxelTextureFiles + resources/textures/voxels/README).
  // Two candidate directories: the runtime copy next to the executable
  // (refreshed at build time) and the working directory (the run scripts
  // launch from the repo root, so freshly added files work without a
  // rebuild).
  {
    std::vector<vv::voxel::VoxelTextureImage> textureImages;
    std::vector<vv::voxel::VoxelTextureSet> textureSets;
    std::string textureLog;
    std::error_code cwdError;
    const auto cwd = std::filesystem::current_path(cwdError);
    const std::vector<std::filesystem::path> textureDirs = {
        vv::core::executableDir() / "resources" / "textures" / "voxels",
        cwdError ? std::filesystem::path{}
                 : cwd / "resources" / "textures" / "voxels"};
    bool anyTextured = false;
    for (const auto& textureDir : textureDirs) {
      if (!vv::render::loadVoxelTextureFiles(textureDir, textureImages,
                                             textureSets, textureLog)) {
        outError = textureLog;
        return false;
      }
      anyTextured = std::any_of(
          textureSets.begin(), textureSets.end(),
          [](const vv::voxel::VoxelTextureSet& set) { return set.textured; });
      if (anyTextured || !textureLog.empty()) {
        std::fprintf(stderr, "[vulkan] textures from %s:\n%s\n",
                     textureDir.string().c_str(), textureLog.c_str());
      }
      if (anyTextured) {
        break;  // first directory with real files wins
      }
    }
    if (!m_voxelResources.createVoxelTextures(
            m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
            textureImages, textureSets, outError)) {
      return false;
    }
  }

  m_slotOf.clear();
  m_freeSlots.clear();
  m_freeSlots.reserve(m_voxelResources.slotCount());
  for (uint32_t slot = m_voxelResources.slotCount(); slot-- > 0;) {
    m_freeSlots.push_back(slot);
  }
  m_slotFadeStart.assign(m_voxelResources.slotCount(), {});
  m_slotFadeScratch.assign(m_voxelResources.slotCount(), 1.0f);
  m_farEverActivated = false;

  // Startup is ASYNC now: the old synchronous initial region blocked the
  // first frame for ~10 s on slow machines (729 chunks x ~15 ms). The far
  // build starts first (it needs the longest head start), then the region
  // streams in through the normal worker + pump path from frame one -
  // the world pops in around the camera over a couple of seconds instead
  // of freezing. The table halves start zeroed (= all empty slots), so
  // the first frames simply show far-LOD terrain everywhere.
  launchFarFieldBuild(0, 0);
  beginRegionMove(0, 0);

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
  const auto perfT0 = std::chrono::steady_clock::now();
  const uint32_t radius = m_voxelConfig.renderRadiusChunks;
  const int32_t r = static_cast<int32_t>(radius);
  struct SyncLog {
    VulkanRenderer& r;
    std::chrono::steady_clock::time_point t0;
    std::size_t generated = 0;
    ~SyncLog() {
      r.m_perfSyncMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      if (r.m_perfEnabled) {
        std::fprintf(stderr, "[perf] SYNC region rebuild: %zu chunks, %.1f ms\n",
                     generated, r.m_perfSyncMs);
      }
    }
  } syncLog{*this, perfT0};

  // Synchronous path (initial region, teleport fallback): a full drain is
  // acceptable here - it also retires every frame that could reference
  // cooldown slots, so they can be recycled immediately.
  vkDeviceWaitIdle(m_device);
  {
    std::lock_guard<std::mutex> lock(m_genMutex);
    m_genRequests.clear();
    m_genResults.clear();
  }
  for (auto it = m_slotCooldown.begin(); it != m_slotCooldown.end();) {
    m_freeSlots.push_back(it->first);
    it = m_slotCooldown.erase(it);
  }
  const uint32_t gridW = m_voxelConfig.gridWidth();
  const uint32_t gridH = m_voxelConfig.gridHeight();
  const int32_t originX = centerChunkX - r;
  const int32_t originZ = centerChunkZ - r;

  std::vector<const vv::voxel::Chunk*> newChunks;
  std::vector<vv::voxel::ChunkCoord> evicted;
  // radius + 1: the seam-patch ring (see rebuildStreamPending) must be
  // generated here too - the far cells one chunk beyond the region edge
  // need real column tops or folded terrain shows holes there.
  m_world->ensureRegion(centerChunkX, centerChunkZ, radius + 1, newChunks,
                        evicted);
  syncLog.generated = newChunks.size();

  for (const vv::voxel::ChunkCoord& coord : evicted) {
    const auto it = m_slotOf.find(coord);
    if (it != m_slotOf.end()) {
      m_slotCooldown.emplace_back(it->second, m_frameCounter);
      m_slotFadeStart[it->second] = {};
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
  // (These capacity-fallback slots are safe to reuse immediately: the
  // device wait at the top of this function already drained everything.)

  if (needUpload.size() > m_freeSlots.size()) {
    outError = "Chunk atlas exhausted (need " +
               std::to_string(needUpload.size()) + " slots, have " +
               std::to_string(m_freeSlots.size()) + " free).";
    return false;
  }

  std::vector<vv::vulkan::VoxelResources::ChunkUpload> uploads;
  uploads.reserve(needUpload.size());
  const auto fadeStart = std::chrono::steady_clock::now();
  for (const auto& [coord, chunk] : needUpload) {
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_slotOf[coord] = slot;
    m_slotFadeStart[slot] = fadeStart;  // teleport regions fade in too
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
  const uint32_t nextTableHalf =
      (m_tableHalf + 1u) % vv::vulkan::VoxelResources::kTableHalves;
  if (!m_voxelResources.writeChunkTable(table, nextTableHalf)) {
    outError = "Failed to update the chunk table.";
    return false;
  }
  m_tableHalf = nextTableHalf;

  // Diagnostic (rare missing-chunk hunt): log empty cells in the table
  // that was just published.
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (table[i] == vv::vulkan::VoxelResources::kEmptySlot) {
      const int32_t gx =
          static_cast<int32_t>(i % m_voxelConfig.gridWidth()) - r;
      const int32_t gz =
          static_cast<int32_t>(i / m_voxelConfig.gridWidth()) - r;
      std::fprintf(stderr, "[vulkan] TABLE HOLE at chunk (%d,%d)\n",
                   centerChunkX + gx, centerChunkZ + gz);
    }
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
  VkDescriptorPoolSize poolSizes[4] = {};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 8;  // voxel atlas, output, chunk table,
                                     // palette, column heights, far LOD,
                                     // chunk fade, texture info table
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[1].descriptorCount = 1;
  poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  poolSizes[2].descriptorCount =
      vv::voxel::kMaxVoxelTextures;  // bindless texture array capacity
  poolSizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  poolSizes[3].descriptorCount = 1;

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

  VkWriteDescriptorSet writes[11] = {};
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

  VkDescriptorBufferInfo fadeInfo{};
  fadeInfo.buffer = m_voxelResources.fadeBuffer();
  fadeInfo.offset = 0;
  fadeInfo.range = VK_WHOLE_SIZE;

  writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[7].dstSet = m_descriptorSet;
  writes[7].dstBinding = 7;
  writes[7].descriptorCount = 1;
  writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[7].pBufferInfo = &fadeInfo;

  // Bindless voxel textures: one image view per VoxelType (binding 8)
  // plus the shared sampler (binding 9).
  // Bind the loaded images; unused capacity slots get the white dummy
  // view so every array element is a valid descriptor.
  const uint32_t textureCount = m_voxelResources.voxelTextureCount();
  std::vector<VkDescriptorImageInfo> textureInfos(
      vv::voxel::kMaxVoxelTextures);
  for (uint32_t i = 0; i < vv::voxel::kMaxVoxelTextures; ++i) {
    textureInfos[i].sampler = VK_NULL_HANDLE;
    textureInfos[i].imageView = m_voxelResources.voxelTextureView(
        i < textureCount ? i : 0u);
    textureInfos[i].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[8].dstSet = m_descriptorSet;
  writes[8].dstBinding = 8;
  writes[8].descriptorCount = vv::voxel::kMaxVoxelTextures;
  writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[8].pImageInfo = textureInfos.data();

  VkDescriptorImageInfo samplerInfo{};
  samplerInfo.sampler = m_voxelResources.voxelSampler();
  samplerInfo.imageView = VK_NULL_HANDLE;
  samplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[9].dstSet = m_descriptorSet;
  writes[9].dstBinding = 9;
  writes[9].descriptorCount = 1;
  writes[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[9].pImageInfo = &samplerInfo;

  VkDescriptorBufferInfo texInfoInfo{};
  texInfoInfo.buffer = m_voxelResources.voxelTexInfoBuffer();
  texInfoInfo.offset = 0;
  texInfoInfo.range = VK_WHOLE_SIZE;
  writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[10].dstSet = m_descriptorSet;
  writes[10].dstBinding = 10;
  writes[10].descriptorCount = 1;
  writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[10].pBufferInfo = &texInfoInfo;

  vkUpdateDescriptorSets(m_device, 11, writes, 0, nullptr);
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

  VkBufferMemoryBarrier preComputeBarriers[4] = {};
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

  // Chunk fade alphas: mapped-memory writes from updateWorld must be
  // visible to the compute stage before the dispatch.
  preComputeBarriers[3].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preComputeBarriers[3].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preComputeBarriers[3].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  preComputeBarriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preComputeBarriers[3].buffer = m_voxelResources.fadeBuffer();
  preComputeBarriers[2].offset = 0;
  preComputeBarriers[2].size = VK_WHOLE_SIZE;

  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 4,
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
  // The origin matches whatever table half is active - during streaming
  // that is the TARGET grid (published incrementally by the pump), not
  // the old region center.
  const int32_t originX =
      m_tableOriginX - static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
  const int32_t originZ =
      m_tableOriginZ - static_cast<int32_t>(m_voxelConfig.renderRadiusChunks);
  // region.w = chunk-table half (ping-pong; the swap path writes the
  // inactive half and flips this index - no device wait).
  push.region = glm::ivec4(originX, 0, originZ,
                           static_cast<int32_t>(m_tableHalf));
  push.grid = glm::uvec4(m_voxelConfig.gridWidth(), m_voxelConfig.gridHeight(),
                         static_cast<uint32_t>(m_voxelResources.slotWordStride()),
                         static_cast<uint32_t>(m_maxTerrainVoxelY));
  if (m_farFieldActive) {
    push.far = glm::ivec4(m_farOriginVoxX, m_farOriginVoxZ,
                          static_cast<int32_t>(m_farDim),
                          static_cast<int32_t>(m_farDim));
    // farParams.y = far-field half (ping-pong; see uploadFarFieldHalf).
    // z/w = VV_DEBUG_HOLE target chunk (z encodes x+4096 as the on-flag).
    float holeZ = 0.0f;
    float holeW = 0.0f;
    if (m_holeDebugX != kHoleDebugOff) {
      holeZ = static_cast<float>(m_holeDebugX + 4096);
      holeW = static_cast<float>(m_holeDebugZ);
    }
    push.farParams = glm::vec4(static_cast<float>(m_farCell),
                               static_cast<float>(m_farHalf), holeZ, holeW);
  } else {
    push.far = glm::ivec4(0, 0, 0, 0);  // z = 0: far LOD off in the shader
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
