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
- `VV_SUN_GRID=0` — disable the pass-26 flood-fill light grid (exact
  binary march fallback). Default on.
- `VV_SUN_MS=<double>` — per-frame CPU build slice for the light grid
  (default 3.0 ms).
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
    src/terrain/{Noise,Noise3D,TerrainGenerator,FarField,SunLightGrid}.cpp \
    src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -o /tmp/t && /tmp/t
```

Toolchain: `sh scripts/build-linux-toolchain.sh "$HOME/.cache/vv-deps"`
(~17 min) and `ln -s ~/.cache/vv-deps /tmp/deps`. NOTE: `~/.cache` is
excluded from workspace snapshots — EVERY sandbox restart needs the
rebuild + reconfigure of build/release and build/debug.

## Current status (pass 26)

Pass 26 (per owner direction after rejecting pass 24's cone/heightmap
soft shadows): regular CPU FLOOD-FILL light grid, GPU reads the texture.
Delivered:

- `src/terrain/SunLightGrid.{hpp,cpp}`: phased incremental builder —
  Prepass (spans/air per column) -> Seeding (cone-table DDA from cell
  centers; exact vs the march on both test fixtures + the real-terrain
  probe: 3.3M exhaustive + 300k random cells, 0 misses) -> Border seeds
  -> Dial-bucket Dijkstra fill (26-conn, x8-quantized direction-weighted
  costs; down-sun 5, up-sun 11, down 8, up 16 per step at azimuth
  (1,1)/sqrt2) -> sliced publish into caller storage. Field = u8 bleed
  distance, budget-independent.
- Renderer: 800x800xH window (origin snapped 256) centered on the
  camera; chunks inside the near region seed from real voxels, columns
  without data from far-LOD heights (same convention as the march).
  Two fields ping-pong along the DEPTH of one 3D R8 texture
  (800x800x2H, binding 11 + own sampler at 12); the served half rides
  in push `farParams.z`, the window origin in `voxelSize.w` (X) and
  `region.y` (Z) — push constants, so no shared-UBO in-flight race.
  `scene.misc.w` = 8*bleed budget (default 14 -> 112) is the shader's
  ramp divisor. `tickSunGrid()` slices the build ~3 ms/frame
  (VV_SUN_MS); requestRebuild() on chunk installs, region moves,
  teleports, far swaps and seam patches; window/sun changes defer to
  the next cycle boundary (a running build stays coherent). The GPU
  copy (staging -> inactive half, GENERAL layout) is recorded in the
  same frame the cycle completes.
- Shader: `sunShadow()` = ONE trilinear fetch when `farParams.w` says
  the field is live (smooth penumbrae, seamless merged shadows);
  `sunRayEscapes` stays as the pre-first-field and VV_SUN_GRID=0
  fallback and remains pinned by the CPU mirror test.
- Tests (`testSunLightGrid`): exhaustive seed parity vs the march
  mirror on two fixtures, boundary exactness, smoothness (max adjacent
  step 0.116 light units), deep-umbra dark, sliced-ticks == one-shot
  build (this caught a real bug: the fill's budget check consumed a
  bucket pop without relaxing it), refresh after geometry change,
  directional step costs (table + sealed-tunnel deltas), all-air
  column agreement, low-sun all-lit. All passing; both builds
  warning-free.
- Also fixed: descriptor pool under-declared its size classes
  (poolSizeCount 2 although sampled/sampler were allocated from;
  count is now 4 with the sun field included).

Owner to verify: soft seamless shadows, no banding, no shadow pops on
window moves, fps impact (VV_PERF=1 shows a `sun` bucket; first field
~1-2 s after startup, then rebuilds only on big moves/installs).

Workspace note: sandbox restarts can return a FRESH CLONE at the base
commit with the working tree preserved (happened twice: before pass 24
and before pass 25). Recovery that worked both times: `git fetch origin
<branch>`, verify the tree matches FETCH_HEAD, `git reset FETCH_HEAD`,
continue. Push often.

Gotchas: tabs (most src) vs 2-space (vulkan/render); edit_file fails on
deep-tab files — use python span edits; heredoc re-typing of code invites
typos — always verify anchors; `ctest`/`cmake` need the PATH export; a
bare-PATH commit silently fails.
