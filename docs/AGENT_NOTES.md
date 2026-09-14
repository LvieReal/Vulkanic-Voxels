# Agent Notes — Vulkanic Voxels

Working notes for AI agent sessions. **The chat is not persistent — read this
first.** Per-pass details live in the git log (one commit per pass, verbose
messages); this file keeps only what you need to work effectively.

## Project

Voxel world ray-traced in a Vulkan compute shader (one thread per pixel,
`resources/shaders/pixels_rgba.comp`), Qt 6 shell, C++23, CMake ≥ 3.26.
Owner's WGSL reference: `docs/reference_renderer.wgsl` (canonical look).

| Path | Contents |
| --- | --- |
| `src/ui/` | Qt shell (`AppWindow`, `VulkanWidget` — swapchain + game loop) |
| `src/platform/` | OS abstraction (Win32/X11/Wayland/macOS) — keep OS headers out of the rest |
| `src/vulkan/` | `VulkanRenderer` (streaming, far LOD, frame loop), `VoxelResources` (GPU buffers) |
| `src/voxel/` | `World`/`Chunk`/`VoxelTypes`/`VoxelConfig` |
| `src/terrain/` | `Noise(2D/3D)`, `TerrainGenerator` (3D density terrain), `FarField` (far LOD + seam patch) |
| `src/render/` | Scene UBO, push constants, lighting config |
| `tests/` | CPU tests (no Qt/Vulkan), `ctest --test-dir build` |
| `scripts/build-linux-toolchain.sh` | Sandbox toolchain (see below) |

## Working rules (owner-set)

- One problem per pass; passes gated on owner verification.
- No TAA (removed pass 9 after owner decision). No SSAA (removed pass 10).
  One ray per pixel.
- User uploads never reach the sandbox — diagnose from code/math/probes.
- Mouse locked always; cross-platform abstraction stays.
- Uncapped fps default; `VV_PRESENT=fifo` restores vsync.
- Keep agent docs/toolchain inside the repo or home — `/tmp` gets wiped
  and the workspace gets rolled back occasionally (recovery: `git fetch
  origin <branch>` + `git reset --soft` + re-commit).

## Key contracts (sync points between CPU and GPU)

- Voxel atlas: 1 byte/voxel, 4 per u32, slot stride = padded chunk bytes;
  layout X + Y*sizeX + Z*sizeX*worldHeight.
- Height atlas: u16 per column (top+1, 0 = air), packed 2 per u32,
  X + Z*sizeX; `(sizeX*sizeZ+1)/2` words per slot.
- Chunk table: u32 slot per region cell, row-major; **triple-buffered**,
  selected by push `region.w`; `kEmptySlot` = air.
- Far field: u32 per cell = height u16 | type u8 << 16; **double-buffered**,
  selected by push `farParams.y`; `pc.far.z = 0` disables far LOD.
- Push constants (128 B): see `src/render/SceneData.hpp` — must match the
  shader's `Push` block exactly.
- Far grid is world-aligned (512-voxel snap): recenters shift the window,
  never re-quantize. Far seam band is patched from real chunk heightmaps
  (`FarField::patchRegion`, incremental, delta-uploaded).
- Streaming: generation on a worker thread (pump installs + fence-scoped
  uploads only); released slots have a 2-frame cooldown; region swaps are
  wait-free (no device/queue waits); teleport fallback stays synchronous.
- Terrain: density = clamp(g·(target−y)) + fbm3·amp, folds (overhangs) in
  mountains; defaults in `TerrainGenerator.hpp` (lift 36, ceiling 100,
  window (0.92, 0.995), snowLine 82). `maxHeightVoxels()` must stay ≤ 127.

## Environment variables

- `VV_DEBUG_TERM` — color miss pixels by ray-termination cause.
- `VV_DEBUG_HOLE=X,Z` — far-miss pixels over chunk (X,Z): magenta = empty
  far cell (data hole), cyan = data present but ray passed over (height
  too low), yellow = march never crossed the chunk.
- `VV_PERF=1` — log frames > 25 ms with the stream/world bucket.
- VV_SHADOW_SHARP=1: exact single-ray sun shadows (no cone penumbra).
- `VV_PRESENT=fifo` — vsync.

## Sandbox validation

```sh
export PATH=/tmp/deps/venv/bin:/tmp/deps/prefix/bin:$PATH   # symlink -> ~/.cache/vv-deps
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake --build build/release && cmake --build build/debug  # warning-free
ctest --test-dir build/release                    # all tests must pass
glslangValidator -V resources/shaders/pixels_rgba.comp -o /tmp/p.spv
QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH=/tmp/deps/qt6/lib:/tmp/deps/prefix/lib \
    timeout 8 ./build/release/bin/game            # must exit cleanly (Vulkan dialog, no crash)
# CPU tests without the toolchain:
g++ -std=c++20 -O2 -ffp-contract=off -I. -Isrc tests/terrain_world_tests.cpp \
    src/terrain/{Noise,Noise3D,TerrainGenerator,FarField}.cpp \
    src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -o /tmp/t && /tmp/t
```

Toolchain: `bash scripts/build-linux-toolchain.sh` installs into
`~/.cache/vv-deps` (survives /tmp wipes) and symlinks `/tmp/deps` to it.

## Current status: REVERTED TO PASS 21 (owner decision)

After pass 26 (CPU flood-fill light grid) failed on the owner's machine
(everything shadowed / 'sideways' shadows; Release-only immediate
crash; Debug and VV_SUN_GRID=0 fine) and could not be root-caused
remotely, the owner asked to return to the last state they consider
stable: pass 21 below (file-based voxel textures, cone-traced soft sun
shadows, chunk fade). The revert commit (ad0aa6b) on top of 5daf98f
restores the tree byte-identical to dda0c0c; the pass-21 CPU test
suite passes.

If the light grid is ever revisited:
- 5daf98f holds the complete pass-26.3 state: SunLightGrid module
  (exhaustively validated on CPU - exact parity vs the march on
  fixtures and real terrain, renderer-scale windows, nonzero origins,
  moved regions, far rings), renderer integration, CrashLog telemetry
  (vv-crash.log next to the exe - the Release exe is -mwindows, its
  stderr is detached!), VV_SUN_DEBUG raw-field view, and a bit-exact
  GPU sampling-path emulation test. All of it passed every CPU test and
  still failed on the owner's GPU - uneliminated suspects: driver-
  specific GENERAL-layout 3D-texture sampling, mingw -O3 codegen, and
  the staging/copy/flip path.
- Salvage-worthy standalone fix inside the reverted range: the pass-13
  stuck m_streamActive bug (finishRegionMove ran every frame at rest;
  fixed in 20ca086). The bug is PRESENT AGAIN in this tree (harmless
  at rest, minor per-frame cost); cherry-pick just that hunk if a later
  pass touches streaming.

Owner's stated next step after the shadow saga: their benchmark /
optimization work (tips delivered in pass 20).
