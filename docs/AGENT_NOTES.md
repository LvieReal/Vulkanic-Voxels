# Agent Notes — Vulkanic Voxels

Notes for AI agent sessions (Arena.ai Agent Mode) working in this repository.
**This chat is not persistent — read this file first.** It records what was
done, how to verify work in the sandbox, and the project's roadmap. Keep it
updated when you finish a pass.

## Project overview

Voxel rendering experiment: a single 64×64×64 voxel chunk ray-traced in a
Vulkan **compute shader** (one thread per pixel, `resources/shaders/pixels_rgba.comp`),
presented through a Qt 6 Widgets shell. C++23, CMake ≥ 3.26.

Code map:

| Path | Contents |
| --- | --- |
| `src/ui/` | Qt shell (`AppWindow`, `VulkanWidget` — hosts swapchain + game loop) |
| `src/platform/` | **Platform abstraction layer** (see below) — keep OS headers out of everything else |
| `src/vulkan/` | Renderer, chunk atlas GPU resources (`VoxelResources`), swapchain, device/surface management |
| `src/voxel/` | World/chunk/voxel data model (`VoxelTypes` palette, `VoxelConfig`, chunked `World`) |
| `src/terrain/` | Deterministic noise (`Noise2D`) + `TerrainGenerator` (fBm heightmap, layered types) |
| `src/render/` | Scene uniforms, lighting config, shader data layouts |
| `src/core/` | Camera, timer, runtime paths, file loading |
| `resources/shaders/` | GLSL compute shaders, compiled to SPIR-V at build time by glslang |
| `tests/` | Pure-logic test suite (no Qt/Vulkan); `ctest --test-dir build` |
| `scripts/` | `build-linux-toolchain.sh` — sandbox build toolchain (see below) |
| `cmake/` | Build functions (GameTarget, Tests, Shaders, Packaging, …) |

### Platform abstraction layer (`src/platform/`)

Flow: `VulkanWidget` → `resolveNativeWindow()` (fills `NativeWindow`, a
Qt/Vulkan-free struct of opaque handles) → `VulkanRenderer` →
`requiredVulkanInstanceExtensions()` + `createVulkanSurface()`.

- Backends: Win32, XCB (X11), Wayland, MoltenVK (macOS). On Linux both XCB and
  Wayland are compiled in when headers exist (`VV_HAVE_XCB` / `VV_HAVE_WAYLAND`,
  decided in `cmake/GameTarget.cmake`) and chosen at run time from the QPA
  platform name.
- All WSI entry points are resolved via `vkGetInstanceProcAddr`, never linked
  directly.
- Qt private headers (`Qt6::GuiPrivate` / the QPA native interface) are
  **optional** (`VV_HAVE_QT_QPA`). Without them: Windows/macOS/X11 use public
  APIs (`QWidget::winId()`, `QNativeInterface::QX11Application`); the native
  Wayland backend is unavailable (error message points to XWayland:
  `QT_QPA_PLATFORM=xcb`). Reason: many Qt distributions (e.g. some MSYS2/MinGW
  setups) ship no private dev files.
- To add a new backend: extend `NativeWindowKind`, `NativeWindow`, the resolver
  (`QtNativeWindowResolver.cpp`), and `VulkanSurfaceFactory.cpp`. Nothing else
  should need touching.

### Input model (as of pass 2)

Mouse is locked to window center (hidden cursor) while playing, with
`setMouseTracking(true)` so look works without holding any button. `Esc`
releases the pointer and pauses the camera; `Esc` or a click re-locks. Focus
loss always releases the pointer and clears key state. Mouse grab goes through
`QWindow::setMouseGrabEnabled` and is deferred to the first Expose event if the
window is not yet visible (this is what removed the old
`setMouseGrabEnabled: Not setting mouse grab for invisible window` warning).

### World architecture (as of pass 2)

- Infinite on X/Z, chunked: 32×128×32-voxel chunks (full world height per
  chunk), CPU cache in `vv::voxel::World` keyed by `ChunkCoord` with
  radius+1 hysteresis eviction (`ensureRegion`).
- Terrain: `src/terrain` — hand-rolled deterministic Perlin-style `Noise2D`
  (integer-hash gradients, no deps; SIMD left for the optimization pass),
  `TerrainGenerator` = hilliness-masked fBm heightmap + layered types
  (grass/dirt/stone, sand under `sandLine`, snow above `snowLine`, bedrock at
  y=0). Same seed → identical terrain on every platform.
- GPU: `vulkan/VoxelResources` = one device-local **chunk atlas** storage
  buffer (one byte per VoxelType, packed 4-per-uint32, slot per chunk) + one
  host-visible **chunk table** SSBO (region grid cell → slot index,
  `kEmptySlot`=0xFFFFFFFF). The renderer (`rebuildChunkRegion`) maps chunk
  coords → atlas slots with a free-list, batch-uploads new chunks (single
  staging buffer + `vkQueueWaitIdle` — rare, only on chunk-border crossings)
  and rewrites the table. `updateWorld(cameraPos)` is called every frame and
  no-ops unless the camera chunk changed.
- Shader `pixels_rgba.comp`: DDA over the region AABB; per-step chunk lookup
  (floor-div coords → table slot → packed word fetch); exponential distance
  fog to sky; sky-skip early-out (rays whose region segment stays above
  `maxHeightVoxels()` can never hit — `push.grid.w` carries the bound, and the
  DDA breaks once ascending above it). Colors come from the **voxel palette
  buffer** (binding 4, filled from `kVoxelTypeInfo` at renderer init) — no
  colors are hardcoded in the shader; a bindless texture array is the planned
  replacement. Push constants must match `vv::render::PushConstants`.
- Default view: render radius 6 chunks (13×13, ~22 MB atlas), fog density
  derived from the radius in `computeFogDensity()`, maxTraceSteps 512.

### GPU error handling (as of pass 2.1)

After a user-reported driver crash ("renderer freezes, no error messages"):
- All `vkAcquireNextImageKHR` / `vkQueueSubmit` / `vkQueuePresentKHR` /
  record failures now set a device-lost state with a message; the widget
  shows one message box and stops rendering (`VulkanRenderer::deviceLost()`).
- Optional `VK_EXT_debug_utils` messenger routes validation messages to
  stderr; the Khronos validation layer is enabled only when the
  `VV_VALIDATION` env var is set and the layer is installed.
- Fixed a real bug found during the investigation: with an exactly
  region-sized atlas, hysteresis eviction lags demand, so the first chunk
  crossing after startup always failed (silently) and the region never
  re-centered. `rebuildChunkRegion` now (a) collects *all* slotless region
  chunks (CPU-cached chunks that lost their slot only need a re-upload), and
  (b) falls back to releasing slots outside the region when free slots run
  short — always makes progress, teleports included.
- TDR mitigation: maxTraceSteps 1024 → 512, sky-skip early-out (pass-1's
  random chunk hit within ~2 steps; air traversal of 400+ cells was the new
  worst case). If device loss persists on weaker GPUs, next steps: reduce
  `renderRadiusChunks`, then the heightmap-guided skipping planned for the
  optimization pass.

## Roadmap status

**Pass 1 — done (commit "Cross-platform platform layer…"):**
- [x] Linux/macOS support via the platform abstraction layer (no unconditional
      `windows.h` anywhere)
- [x] Scraped the Qt escape/pause menu (native in-engine UI comes later);
      Escape now toggles pointer lock + pause
- [x] Fixed the mouse-grab warning (deferred grab, see above)
- [x] README with per-OS build instructions
- [x] Fix: `setMouseTracking(true)` so first-person look needs no button held
- [x] Docs kept in-repo (this file + toolchain script) per user request

**Pass 2 — done (pending user verification):**
- [x] Voxel types (grass, dirt, stone, sand, snow, bedrock) with per-face
      colors instead of random colors
- [x] Terrain generation with noise (hand-rolled scalar Perlin fBm; pulling a
      web library was deemed unnecessary — revisit for SIMD pass)
- [x] Infinite worlds via chunking (X/Z only, full-height chunks, GPU chunk
      atlas + region management, distance fog)
- [x] Pure-logic test suite (`tests/`, runs headless in sandbox)

**Pass 2.1 — done (crash investigation after user report):**
- [x] Region slot-exhaustion bug fixed (first border crossing always failed
      silently; see "GPU error handling" above)
- [x] Vulkan errors surfaced (device-lost state + UI message box); previously
      every GPU error was swallowed — matched the "no error messages" report
- [x] TDR mitigation: maxTraceSteps 512, sky-skip early-out
- [x] Debug messenger (stderr) + opt-in validation layer via VV_VALIDATION=1
- [x] Palette moved out of the shader into a data buffer (binding 4); note
      added that a bindless texture array is planned

**Pass 3 — planned, START ONLY AFTER USER VERIFIES PASS 2:**
- Optimizations: SIMD noise, bitwise ops, faster traversal for large worlds
- Infinite render distance via level of detail
- (Later) native in-engine UI instead of Qt widgets for the escape menu

## Sandbox facts (Arena.ai environment)

- **`/tmp` is ephemeral between prompts.** Anything placed there (including the
  whole Linux toolchain) can vanish between messages. Only the workspace
  (`/home/user/Vulkanic-Voxels`) persists, via snapshots — and snapshot-capped
  directories (`build/`, `.cache/`, `node_modules/`, …) do not persist either.
  Don't rely on a previous prompt's build tree existing; re-run the toolchain
  script and rebuild when in doubt.
- **Network:** only `github.com` / `codeload.github.com` / `api.github.com` /
  `pypi.org` are reachable. `apt` and Debian mirrors are blocked,
  freedesktop.org is unreachable (no real xcb/wayland headers downloadable —
  the toolchain script installs ABI-identical compile-validation shims
  instead, clearly marked inside the files).
- **No GPU / no display server.** Never expect the game to actually render in
  the sandbox; validate that it *builds, links, and fails gracefully* at run
  time.

### Verifying a Linux build in the sandbox

```sh
# One-time (or after /tmp was wiped), ~20-30 min on 2 cores:
scripts/build-linux-toolchain.sh /tmp/deps

export PATH="/tmp/deps/venv/bin:/tmp/deps/prefix/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake --build build            # must be warning-free
ctest --test-dir build         # terrain/world logic tests (no GPU needed)
cmake --build build --target package_folder   # packaging check

# Headless smoke test: no GPU -> the game must show the graceful
# Vulkan error dialog (modal, so it runs until timeout) instead of crashing.
QT_QPA_PLATFORM=offscreen \
LD_LIBRARY_PATH="/tmp/deps/qt6/lib:/tmp/deps/prefix/lib" \
    timeout 5 ./build/bin/game
```

The script builds: pip CMake 3.31 + Ninja, zlib, minimal Qt 6.8.3
(offscreen/minimal QPA only — no xcb/OpenGL in Qt itself), Vulkan headers +
loader (WSI off; WSI functions resolved at run time), glm, glslang, and the
xcb/wayland compile shims. It is idempotent — re-running skips finished parts.

### Session conventions

- Work happens on the session branch (`arena/…`, shown in the environment);
  never touch other branches. Commit per pass; push with
  `git push origin <session-branch>`.
- `git push` / `gh` can fail with an authentication error when the session
  token expires (observed after ~1h). Retry on a later turn; if it persists,
  ask the user to reconnect GitHub in Arena settings.
- **Never issue parallel `edit_file` calls against the same file** — they
  race and silently drop each other's changes (observed in pass 2). Parallel
  edits to *different* files are fine.
- `/tmp` can be wiped **mid-turn**, not just between prompts (observed in
  pass 2): don't cache toolchain assumptions even within one turn.
- Code style: `.clang-format` is Google-based, **tabs**, 2-width. New UI/core
  files follow it; the older `vulkan/` and `render/` files use 2-space indent
  (don't mass-reformat, match the file you edit).

### Known caveats

- The sandbox Qt is built without xcb, so the resolver's public X11 fallback
  (`QNativeInterface::QX11Application`) cannot be compile-tested there beyond a
  manual `-DQT_FEATURE_xcb=1` object-file check; real X11 runs happen on user
  machines.
- Wayland rendering into a QWidget is best-effort (needs Qt private headers);
  X11 is the battle-tested path.
- macOS and Windows runtime behavior is verified by the user, not the sandbox.
