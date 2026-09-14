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

## Current status (pass 27)

Tree state: pass-25 shadows RESTORED on top of the pass-21 revert (the
owner confirmed the revert worked, then asked for the cone tracing to
go too): file-based voxel textures + pass-22 aliases intact, sun
shadows back to the single exact binary march, no light grid, no cone
machinery. Owner-verified base: 3ff725f.

Pass 29 (textures): SIDE-FACE V AXIS FLIPPED - textures displayed
upside down on every side face. Root cause: the shader built side-face
UVs as (z,y)/(x,y), i.e. V = world +Y, but in this pipeline Vulkan's
V=0 is the image's TOP row (QImage rows upload in order), so the image
top landed at the voxel BOTTOM. Fix: V = -Y on side faces (REPEAT
sampler; fract(-y) = 1 - fract(y) keeps tiling continuous). Top/bottom
faces keep V = +Z (rotation, not flip - art-dependent). README UV
paragraph updated ("textures display UPRIGHT on sides; mirror the file
only for horizontal direction"). Validated: both builds warning-free
(shader recompiled), ctest green, smoke ok.

Pass 28 (textures): PARTIAL TEXTURE SETS NOW WORK PER FACE. Owner
report: grass_top.png + grass_side.png + "grass bottom = dirt" in
aliases.txt still rendered grass as plain colors. Root cause: a mode
only applied when ALL its files existed (the README's own example was
impossible), so grass had no complete side-uniform set -> entirely
plain, and the alias then textured only the rarely-visible bottom face.
Fix: per-face resolution - each face independently uses the first of
its candidate files that exists (top/bottom: _top/_bottom then the
uniform file; side faces: the custom name, then _side, then uniform);
unresolved faces stay plain or take an alias. The startup log now says
per type "N files, M/6 faces (missing: bottom ...)" so gaps point at
their fix. Pure logic (suffix chains, face-name mapping, alias-source
resolution) moved to voxel/VoxelTextures.hpp and unit tested incl. the
exact reported scenario. README/header docs updated. Validated: both
builds warning-free, ctest green, smoke ok.

Pass 27 (first of the fix-one-by-one round): STREAMING PRIORITY WAS
BACKWARDS. The pump stocks m_genRequests by iterating the sorted
pending list in reverse (best first), but the workers popped the BACK -
so the best coords sat at the FRONT forever while every top-up
(appended at the back, always a little worse) was generated first: the
nearest, in-frustum chunks appeared dead last after every region move.
Fix: the workers consume the FRONT (strict FIFO in descending
priority). streamPriority() extracted to src/vulkan/StreamPriority.hpp
(glm-free) and pinned by testStreamPriority, including a full
pump/worker queue simulation (old behavior: 26 priority inversions;
fixed: 0). Validated: both builds warning-free, ctest green, smoke ok.

Note for the fix round: each fix = one pass = one commit, owner
verifies before the next.

Owner's stated next step after the shadow saga: their benchmark /
optimization work (tips delivered in pass 20).

Pass 30 (first optimization pass, owner-requested): HIERARCHICAL DDA.
New per-chunk "block max" atlas: one u16 per 8x8 block of columns = the
MAX column bound in the block (kHeightBlockVoxels in voxel/VoxelTypes.hpp
== kBlockVoxels in the shader; 0 = all-air block), packed two per u32,
8 words per 32x32 slot, uploaded with each chunk (binding 11, the slot
freed by the light-grid revert). In the march's column loop: on entering
a new block, one fetch decides whether the ray provably stays above
every column of the block until it exits - if so the whole block is
crossed with branchless ALU-only DDA steps (no chunk-table resolve, no
column-height fetch, no Y-walk per column). Safety: block exit
mid-column resumes exactly at the exit t; a re-entry guard handles
ulp-level boundary misses (stops skipping that block, normal column
logic advances out); kNoHeightData blocks are never skipped. The skip
condition implies each skipped column's own air-skip condition, so the
image is bit-identical. CPU mirror traceHier + 3-way parity
(old/new/hierarchical, 6000 rays x 2 worlds) green; iteration counts:
world 0 84,033 -> 36,259 (-57%), world 1 69,924 -> 33,668 (-52%).
Block-map packing unit tests added (single-block chunk, 9x9 round-up,
generated 32x32 chunks vs ground truth).
