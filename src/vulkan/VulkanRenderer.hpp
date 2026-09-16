#pragma once

#include <vulkan/vulkan.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Camera.hpp"
#include "platform/NativeWindow.hpp"
#include "render/LightingConfig.hpp"
#include "render/SceneUniform.hpp"
#include "terrain/FarField.hpp"
#include "voxel/VoxelConfig.hpp"
#include "voxel/World.hpp"
#include "vulkan/VoxelResources.hpp"

namespace vv::vulkan {

class VulkanRenderer final {
 public:
  struct InitInfo {
    // Platform-agnostic description of the native window to render into.
    // See vv::platform::NativeWindow and platform/GlfwNativeWindow.
    vv::platform::NativeWindow nativeWindow;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  VulkanRenderer() = default;
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  bool init(const InitInfo& info, std::string& outError);
  void resize(uint32_t width, uint32_t height);

  // Size of the swapchain currently in use (the extent the driver reported on
  // surface creation; compare against the window's framebuffer size when a
  // rendering report has to be diagnosed).
  uint32_t swapchainWidth() const { return m_swapchainExtent.width; }
  uint32_t swapchainHeight() const { return m_swapchainExtent.height; }
  void drawFrame();
  void setCamera(const vv::core::Camera& camera, float timeSeconds);
  void setWorldConfig(const vv::voxel::VoxelConfig& config);

  // Keeps the GPU chunk region centered on the camera. Cheap no-op unless the
  // camera crossed a chunk boundary; call every frame before drawFrame().
  // Border crossings stream the new region in incrementally (time-sliced
  // generation + small tear-free uploads into spare atlas slots); the old
  // region keeps rendering until the new one is complete.
  void updateWorld(const glm::vec3& cameraPosition);

  // Far-LOD field lifecycle: launches background builds when the camera
  // strays too far from the active field, and uploads finished builds.
  // Called from updateWorld; needs the queue for uploads.
  void ensureFarField(int32_t centerChunkX, int32_t centerChunkZ);
  void launchFarFieldBuild(int32_t centerChunkX, int32_t centerChunkZ);

  // 3D voxel SDF (pass 38, VV_SDF_SHADOWS=1): a background build of the
  // nearest-solid-cell field over a camera-centered box (built on the CPU
  // with vv::voxel::SdfField, the same reference the CPU test pins).
  // launchSdfBuild is called when a region move completes (the world
  // chunks under the box are then installed); ensureSdfField (called from
  // updateWorld) joins a finished build and uploads it, publishing the box
  // uniform the shader reads.
  void ensureSdfField();
  // Pass 49: arm a rebuild when the camera's chunk has drifted past the live
  // field's margin (never while a region move is streaming).
  void followSdfField(int32_t chunkX, int32_t chunkZ);
  void launchSdfBuild(int32_t centerChunkX, int32_t centerChunkZ);

  // Incremental region streaming. Generation runs on a WORKER thread
  // (pass 13): the render thread only installs finished chunks and uploads
  // them (sub-millisecond) - generation (~2.8 ms/chunk, up to 4/frame at
  // sprint) on the render thread was the last main-thread hitch source.
  void beginRegionMove(int32_t targetChunkX, int32_t targetChunkZ);
  // Worker plumbing (see the .cpp for the memory-ordering rationale).
  struct GeneratedChunk {
    vv::voxel::ChunkCoord coord;
    std::vector<std::uint8_t> types;
  };
  void generationWorker(const vv::terrain::TerrainGenerator* gen);
  void stopGenerationWorker();
  // Installs finished chunks, tops up requests from the pending set.
  void pumpRegionStreaming();
  void rebuildStreamPending();

  void finishRegionMove();
  // Incremental chunk-table publish (see the .cpp).
  void publishRegionTable(bool logHoles);

  // Sliced far-LOD seam patch (see the .cpp): a few chunks per frame,
  // extent grows only over chunks actually scanned. Folded mountains can
  // under-estimate by 20+ voxels -> holes at the seam without this.
  std::size_t drainFarPatch();

  // Suggested camera spawn: above the terrain at the center of chunk (0,0).
  glm::vec3 spawnPosition() const;

  // One-line live state for the window title / stderr: fog cut distance,
  // fog density, step budget, region grid and slot usage. Used to debug
  // rendering reports remotely (a stale binary shows stale numbers).
  std::string debugStats() const;

  // After a device loss / fatal Vulkan error, drawFrame() becomes a no-op and
  // the reason is available here for the UI to surface.
  bool deviceLost() const { return m_deviceLost; }
  const std::string& lastError() const { return m_lastError; }

  // New: allow external configuration of lighting (separated concern).
  void setLighting(const vv::render::LightingConfig& lighting) {
    m_lighting = lighting;
  }
  const vv::render::LightingConfig& lighting() const { return m_lighting; }
  const vv::voxel::VoxelConfig& voxelConfig() const { return m_voxelConfig; }

 private:
  void cleanup();

  bool createInstance(const InitInfo& info, std::string& outError);
  bool createSurface(const InitInfo& info, std::string& outError);
  bool pickPhysicalDevice(std::string& outError);
  bool createDevice(std::string& outError);

  bool createSwapchain(uint32_t width, uint32_t height, std::string& outError);
  void cleanupSwapchain();
  bool recreateSwapchain(uint32_t width, uint32_t height,
                         std::string& outError);
  // m_requested* with a sane floor (never 0: vkCreateSwapchainKHR rejects a
  // zero extent).
  uint32_t requestedWidth() const;
  uint32_t requestedHeight() const;
  // True when the surface has no size to build a swapchain for (a minimized
  // window on Win32 reports a currentExtent of (0, 0)).
  bool surfaceHasNoSize() const;

  bool createVoxelWorldAndUpload(std::string& outError);
  // Generates/evicts chunks for the new region center, uploads new chunk
  // data into free atlas slots and rewrites the chunk table.
  bool rebuildChunkRegion(int32_t centerChunkX, int32_t centerChunkZ,
                          std::string& outError);
  // Distance from the camera to the nearest SIDE face of the loaded chunk
  // region (world units). This is the fog cut distance: the shader makes the
  // fog 99.8% opaque exactly here and terminates rays beyond, so the box
  // boundary of the region is never visible (every ray's region exit happens
  // at >= this distance along its direction).
  float fogCutDistance() const;
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
  // (Re)creates one render-finished semaphore per swapchain image; called from
  // createSwapchain so the array always matches the images in use.
  bool createPresentSemaphores(std::string& outError);

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
  // The window size the swapchain should have: set on init and on every
  // resize(). The recreate paths in drawFrame() use these, NOT the previous
  // extent - recreating for the old size left the launch frame stretched
  // until the window was touched (pass 45).
  uint32_t m_requestedWidth = 0;
  uint32_t m_requestedHeight = 0;
  // Set while the surface has no size and rebuilds are being deferred, so the
  // note is printed once per episode instead of once per attempt.
  bool m_swapchainRebuildDeferred = false;
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
  // Per swapchain image (see drawFrame), NOT per frame in flight: a present
  // that still waits on one must not be reused with another image.
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

  // World layout (chunk dims, render radius, seed, ...).
  vv::voxel::VoxelConfig m_voxelConfig{};

  // CPU-side world: terrain-generated chunk cache keyed by chunk coords.
  std::unique_ptr<vv::voxel::World> m_world;

  // GPU-side world: chunk atlas + chunk table (see vulkan/VoxelResources).
  vv::vulkan::VoxelResources m_voxelResources;

  // Chunk coord -> atlas slot; free slots are the reuse pool.
  std::unordered_map<vv::voxel::ChunkCoord, uint32_t, vv::voxel::ChunkCoordHash>
      m_slotOf;
  std::vector<uint32_t> m_freeSlots;
  vv::voxel::ChunkCoord m_regionCenter{};

  // --- Incremental region streaming state ---
  // While streaming, the ACTIVE region (m_regionCenter + table) keeps
  // rendering; chunks for the TARGET region generate time-sliced and upload
  // into spare slots the active table never references. The table swaps once
  // when the target region is complete.
  bool m_streamActive = false;
  vv::voxel::ChunkCoord m_streamTarget{};
  // Pending coords, sorted WORST-first (the pump stocks the generation
  // backlog from the REVERSE end; the workers front-consume, so the
  // best coords - frustum-facing, near - generate first). Coords
  // already in m_slotOf are excluded.
  std::vector<vv::voxel::ChunkCoord> m_streamPending;
  // Generate-only coords for the far seam patch (r+1 ring; no slots).
  std::vector<vv::voxel::ChunkCoord> m_streamRingPending;
  // Async generation workers (see pumpRegionStreaming). Two: a single
  // worker produces ~68 chunks/s on a slow CPU - right at the sprint
  // drain rate - so a second keeps the deficit shrinking.
  static constexpr std::size_t kGenWorkers = 2;
  std::vector<std::thread> m_genThreads;
  std::mutex m_genMutex;
  std::condition_variable m_genCV;
  std::vector<vv::voxel::ChunkCoord> m_genRequests;
  std::vector<GeneratedChunk> m_genResults;
  bool m_genStop = false;
  float m_fogDensity = 0.01f;

  // Debug visualization (see AGENT_NOTES): VV_DEBUG_TERM colors each pixel
  // by ray-termination cause. Default off. (VV_DEBUG_SSAA / VV_SSAA were
  // removed in pass 10 - supersampling was too heavy.)
  bool m_debugTerminators = false;
  // Optional SDF soft-shadow experiment (pass 33: the exact march's
  // traversal + the plain Quilez k*h/t penumbra estimate - pass 34; the
  // Aaltonen two-sphere refinement was removed, it projected a hard
  // "clamped edge" stripe). Exact binary shadows remain the default;
  // VV_SDF_SHADOWS=1 selects the soft marcher.
  bool m_sdfShadows = false;
  // VV_PERF: frame-time logging for hitch diagnosis (frames > 25 ms,
  // rate-limited). VV_DEBUG_HOLE: color far-march misses over a chunk.
  bool m_perfEnabled = false;
  std::chrono::steady_clock::time_point m_perfLastFrame{};
  std::chrono::steady_clock::time_point m_perfLastLog{};
  double m_perfWorldMs = 0.0;
  double m_perfPumpMs = 0.0;
  double m_perfSyncMs = 0.0;
  double m_perfFarMs = 0.0;
  double m_perfGpuMs = 0.0;
  std::int32_t m_holeDebugX = 0x7FFFFFFF;
  std::int32_t m_holeDebugZ = 0x7FFFFFFF;


  // --- Far-LOD field (background build + upload state) ---
  // The builder thread only touches m_farPending (sole ownership until
  // m_farPendingReady flips to true) and reads the const terrain generator;
  // the main thread joins it in cleanup() before the world is destroyed.
  std::thread m_farThread;
  std::atomic<bool> m_farBuildRunning{false};
  std::atomic<bool> m_farPendingReady{false};
  vv::terrain::FarField m_farPending;
  // Chunk the in-flight build is centered on (becomes the active center on
  // upload).
  std::int32_t m_farPendingCenterX = 0;
  std::int32_t m_farPendingCenterZ = 0;
  // Active (uploaded) field geometry: origin in voxels, cell count and
  // footprint; valid only while m_farFieldActive. m_farCells mirrors the
  // ACTIVE half on the CPU so patchFarFieldWithRegion can rewrite the
  // seam band incrementally. m_farHalf is the push-constant half index.
  std::int32_t m_farOriginVoxX = 0;
  std::int32_t m_farOriginVoxZ = 0;
  std::uint32_t m_farDim = 0;
  std::uint32_t m_farCell = 0;
  bool m_farFieldActive = false;
  std::vector<std::uint32_t> m_farCells;
  std::uint32_t m_farHalf = 0;
  // Chunk-coordinate box already patched into m_farCells (invalid when
  // MaxX < MinX, e.g. after a far-field activation).
  std::int32_t m_farPatchMinX = 0;
  std::int32_t m_farPatchMinZ = 0;
  std::int32_t m_farPatchMaxX = -1;
  std::int32_t m_farPatchMaxZ = -1;
  // Active chunk-table half (push-constant index; triple-buffered) and
  // the chunk-grid origin of the table content in that half (during
  // streaming this is the TARGET grid, published incrementally).
  std::uint32_t m_tableHalf = 0;
  std::int32_t m_tableOriginX = 0;
  std::int32_t m_tableOriginZ = 0;
  // Slots released by a region swap, waiting until no in-flight frame can
  // reference them (two frames) before rejoining the free list.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> m_slotCooldown;
  // Chunk fade-in (pass 19): per-slot fade start times (epoch = opaque);
  // m_slotFadeScratch is the per-frame alpha array pushed to binding 7.
  // The far fade ramps once, after the FIRST far-field activation.
  std::vector<std::chrono::steady_clock::time_point> m_slotFadeStart;
  std::vector<float> m_slotFadeScratch;
  bool m_farEverActivated = false;
  std::chrono::steady_clock::time_point m_farFadeStart{};

  // --- 3D voxel SDF (pass 38, VV_SDF_SHADOWS=1; background build) ---
  // The builder thread only touches m_sdfPending (sole ownership until
  // m_sdfPendingReady flips to true) and reads the world chunks under the
  // box (installed by the time the build launches); the main thread joins
  // it in cleanup() before the world is destroyed, and uploads the field
  // in ensureSdfField.
  std::thread m_sdfThread;
  std::atomic<bool> m_sdfBuildRunning{false};
  std::atomic<bool> m_sdfPendingReady{false};
  struct SdfBuild {
    std::vector<std::uint32_t> seeds;  // argmin seed per cell (box layout,
                                       // kSdfEmptySeed = no solid in view)
    std::int32_t boxX = 0;  // box origin in world voxels
    std::int32_t boxY = 0;
    std::int32_t boxZ = 0;
    std::uint32_t nx = 0, ny = 0, nz = 0;  // box size in cells (banded ny)
    std::uint32_t fullNy = 0;  // uncropped height (publish-path sanity check)
    // Pass 50: the seed packing's bits per axis (vv::voxel::SdfField::SeedBits)
    // - the shader needs them to decode a seed with shifts and masks.
    std::uint32_t seedBitsX = 0, seedBitsY = 0, seedBitsZ = 0;
    std::int32_t centerChunkX = 0;  // the build's center (stale check)
    std::int32_t centerChunkZ = 0;
    // Pass 49 (VV_PERF): where a bake's time went. band/build are worker
    // thread, snapshot/upload are render thread.
    double snapshotMs = 0.0;
    double bandMs = 0.0;
    double buildMs = 0.0;
  };
  SdfBuild m_sdfPending;
  // --- SDF handover state (pass 42) ---
  // The box uniform and the seed buffer are a PAIR (the shader resolves
  // "which cell" from the box and indexes the seeds with it), so the publish
  // order matters: vv::voxel::SdfHandover (voxel/SdfHandover.hpp) owns the
  // rules, ensureSdfField drives them. m_sdfUploadInFlight: a copy is on the
  // queue and the box has NOT been published for it; m_sdfUploadBox: the
  // geometry to publish when it lands; m_sdf*Center*: the chunk the LIVE
  // field was built for vs the chunk the camera is on now (the retry lever -
  // a launch swallowed by a running build used to be lost until the next
  // region move, leaving the soft shadows a whole crossing behind).
  struct SdfBoxUpload {
    std::int32_t boxX = 0;
    std::int32_t boxY = 0;
    std::int32_t boxZ = 0;
    std::uint32_t nx = 0, ny = 0, nz = 0;
    std::uint32_t seedBitsX = 0, seedBitsY = 0, seedBitsZ = 0;
    std::int32_t centerChunkX = 0;
    std::int32_t centerChunkZ = 0;
    std::uint32_t half = 0;  // seed half this build was copied into
  };
  bool m_sdfUploadInFlight = false;
  // The seed half the live box points at (the shader's dims.w). The copy
  // fills the other one; vv::voxel::SdfHandover::uploadHalf owns that rule.
  std::uint32_t m_sdfLiveHalf = 0;
  SdfBoxUpload m_sdfUploadBox;
  bool m_sdfFieldActive = false;
  bool m_sdfWantValid = false;
  std::int32_t m_sdfActiveCenterX = 0;
  std::int32_t m_sdfActiveCenterZ = 0;
  std::int32_t m_sdfWantCenterX = 0;
  std::int32_t m_sdfWantCenterZ = 0;
  // Pass 49: how far (in chunks, max-norm) the camera may drift from the live
  // field's center before a rebuild is armed - VV_SDF_MARGIN, default 1, and
  // the box only covers kSdfHalfChunks in each direction. 0 = the pre-49
  // "every completed region move" cadence.
  std::uint32_t m_sdfMarginChunks = 1;
  // Pass 57: the shadow-ray jitter handed to the shader each frame
  // (VV_SHADOW_JITTER) - the CONE SLOPE. < 0 = unset, so the shader's
  // kShadowJitterDefault applies; 0 = jitter off, bit-identical to the
  // un-jittered estimate.
  float m_shadowJitter = -1.0f;
  // Pass-49 bake bookkeeping for the VV_PERF line: how many bakes, when the
  // last one published (bakes/second), and what the render thread paid.
  std::uint64_t m_sdfBakeCount = 0;
  std::chrono::steady_clock::time_point m_sdfLastPublishTime{};
  double m_sdfUploadMs = 0.0;

  // Chunk the active field is centered on (recenter decision).
  std::int32_t m_farCenterChunkX = 0;
  std::int32_t m_farCenterChunkZ = 0;

  // Upper bound on terrain height (voxels); rays above it can never hit.
  std::int32_t m_maxTerrainVoxelY = 0;

  // Fatal-error state (device loss etc.).
  bool m_deviceLost = false;
  std::string m_lastError;

  // Optional VK_EXT_debug_utils messenger for validation-layer messages.
  VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

  void setDeviceLost(const std::string& message);

  // Scene uniform (camera + lighting) separated into SceneUniform utility.
  vv::render::SceneUniform m_sceneUniform;

  // Lighting properties separated into LightingConfig utility.
  vv::render::LightingConfig m_lighting{};
};

}  // namespace vv::vulkan
