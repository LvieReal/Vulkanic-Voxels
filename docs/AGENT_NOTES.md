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

Toolchain: `sh scripts/build-linux-toolchain.sh "$HOME/.cache/vv-deps"`
(~17 min) and `ln -s ~/.cache/vv-deps /tmp/deps`. NOTE: `~/.cache` is
excluded from workspace snapshots — EVERY sandbox restart needs the
rebuild + reconfigure of build/release and build/debug.

## Current status (pass 25)

Owner report on pass 24 (heightmap + penumbra ring): "no success, it got
worse, it also crashes randomly after a couple of seconds". Owner
decision: REMOVE the cone-traced/soft shadow machinery entirely and try
flood fill instead. Pass 25 is the removal; soft shadows are gone until
the flood-fill pass lands.

Delivered (pass 25):
- Deleted `src/terrain/SunShadowMap.{hpp,cpp}`, binding 11 (layout count
  12 -> 11, writes 12 -> 11, pool 10 -> 8), `uploadSunShadowField` +
  buffer + staging/fence, all renderer hooks and members, the shader's
  whole soft path, `VV_SHADOW_SHARP`, and `scene.misc.w` (coneTan).
- `sunShadow()` is back to the pass-20 semantics: ONE exact binary ray
  (`sunRayEscapes`) for every hit, near AND far-LOD. No soft mode.
- Tests: mirror machinery for soft paths removed; `testSunShadowMarch`
  (exact march vs dense brute) restored to direct mirror calls and
  passing (809 lit / 2191 shadowed agree). All other tests unchanged.

Next (agreed direction, NOT yet wired): CPU flood-fill sun light grid.
Prototype validated in /home/user/floodprobe (outside the repo):
- SEEDS: air cells whose exact sun ray escapes get light 1.0 — the
  shadow boundary stays EXACT (0 violations vs the exact march; the
  anisotropic-DP smearing dead end cannot recur because direct light
  never propagates).
- BLEED: Dial bucket-queue Dijkstra through air only; horizontal step
  cost 1 (uniform) or 1 + 0.5*dot(step, sunXZ) (down-sun cheap,
  up-sun 1.5x); down 1, up 2. Light = 1 - dist/budget. Smooth (max
  adjacent step 44/255 at budget 14, zero hard steps), seamless
  (distance field, merged wall+tower shadows continuous), fast
  (~15-40 ms for the 64x64x48 fixture incl. seeding, on 2 contended
  cores). Budget = penumbra width knob (5 = tight/steep, 14 = wide).
- GPU story: 3D R8_UNORM texture (800x800x128 = 82 MB), ONE filtered
  fetch per pixel replaces the whole march; trilinear gives sub-cell
  smoothness on top of the cell grid.

Workspace note: sandbox restarts can return a FRESH CLONE at the base
commit with the working tree preserved (happened twice: before pass 24
and before pass 25). Recovery that worked both times: `git fetch origin
<branch>`, verify the tree matches FETCH_HEAD, `git reset FETCH_HEAD`,
continue. Push often.

Gotchas: tabs (most src) vs 2-space (vulkan/render); edit_file fails on
deep-tab files — use python span edits; heredoc re-typing of code invites
typos — always verify anchors; `ctest`/`cmake` need the PATH export; a
bare-PATH commit silently fails.
