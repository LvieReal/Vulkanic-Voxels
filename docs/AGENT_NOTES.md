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

## Current status (pass 24)

Owner report on pass 23 (9-ray cone): gaps/distortions fixed BUT "very
slow" and "shadows look scattered, no smooth gradient". Pass 24 replaces
the soft path with a CPU-precomputed SUN SHADOW HEIGHTMAP - soft
shading is now a bounded handful of buffer fetches per pixel, cost
independent of blocker distance, with continuous gradients.

Delivered (pass 24):
- `src/terrain/SunShadowMap.{hpp,cpp}`: `SunShadowBuilder` - parallel
  light-ray splatting over the region's columns. Each ray (from the
  up-sun grid edges, 0.25/0.75 sub-cell offsets) carries a running
  shadow height hRun = max(tops seen) decayed by sunY per ray parameter
  (tMax-delta between crossings; a two-axis DP was tried and REJECTED -
  it smears shadows diagonally). Writes per column the PAIR (H, D):
  H = the shadow ceiling at the column; D = distance from the winning
  blocker to the column, quantized /4 in u16 (0 = the column's own top
  won). Incremental tick(crossings); double-buffered; requestRebuild()
  on chunk installs; shiftField on region moves. Validated +-1.0 vs a
  9-offset brute-sup oracle in all four sun quadrants.
- Binding 11 (set 0): [H grid][D grid], each 800x800 u16 packed two per
  u32 along X (2.56 MB device-local, staging+fence upload on completed
  build cycles; zero-initialized at startup so shading starts defined).
- Renderer: tops mirror (`m_sunShadowTops`, Chunk::heightMap with far
  fallback), sliced build (tick(64) per frame in updateWorld), publish
  on cycle completion, shift + exposed-band refresh on table publishes,
  full re-assembly on teleports, reconfigure on sun-direction change.
- Shader soft path: short 8-column march of the receiver's EXACT ray
  (entry knives - resolves close terrain-step grazes that bilinear
  sampling cannot see) + a penumbra ring of 8 bilinear coverage
  samples at radius coneTan*s_b around the center direction's LANDING
  point (not the receiver!), all at entry = s_b. Knife = the same
  formula as the far tier (climb-corrected, vertical spread
  2*tan*sB*|sun.xz|), so near/far penumbrae join seamlessly. Far-LOD
  hits: far cells -> one field test at the region boundary column ->
  far cells (3 legs; visibility combines with min). Sharp mode
  (coneTan 0 / VV_SHADOW_SHARP=1) = the single exact march, unchanged.
- Tests: sharp parity 809/2191 agree; cone vs 13-dir dense reference
  mean |diff| 0.031, 95.9% within 0.25, 1.6% beyond 0.5, 0.9%
  graze-inconsistent (a column-quantized field cannot resolve
  sub-column graze lines - bounded dither, bars set accordingly);
  lateral profile tracks the reference smoothly (max 0.325, no gaps,
  no hard steps); splat vs brute max dev 1.0.

Workspace note: this sandbox is a FRESH CLONE at 240f625 (the pass-14
to 23 commits were lost with the previous workspace); all session work
through pass 24 is committed together from the working tree.

Gotchas: tabs (most src) vs 2-space (vulkan/render); edit_file fails on
deep-tab files — use python span edits; heredoc re-typing of code invites
typos — always verify anchors; `ctest`/`cmake` need the PATH export; a
bare-PATH commit silently fails.
