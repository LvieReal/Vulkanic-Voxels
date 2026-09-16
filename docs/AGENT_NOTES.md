# Agent Notes — Vulkanic Voxels

Working notes for AI agent sessions. **The chat is not persistent — read this
first.** The other documents:

| File | What it is |
| --- | --- |
| `docs/PASSES.md` | shipped-feature reports, one per pass (oldest first) |
| `docs/UPGRADE_PROPOSALS.md` | proposals for future work (ambient light for caves, GPU SDF bake) — not shipped |
| `git log` | one commit per pass, verbose messages: the commit-level detail |

This file keeps only what you need to work effectively: the tree map, the
contracts, the switches, the sandbox recipes, the standing owner rules, and where
the tree stands.

## Project

Voxel world ray-traced in a Vulkan compute shader (one thread per pixel,
`resources/shaders/voxels.comp` — its name since pass 59; older pass reports call
it `pixels_rgba.comp`). C++23, CMake ≥ 3.26, GLFW windowing, no toolkit of any
kind. The owner's WGSL reference renderer is `docs/reference_renderer.wgsl` and
it is the canonical look.

| Path | Contents |
| --- | --- |
| `src/core/` | `App` (window + frame loop + input), `GameWindow`, `InputBindings`, `Version.h.in` |
| `src/platform/` | OS abstraction (Win32/X11/Wayland/macOS) — keep OS headers out of the rest of the tree |
| `src/vulkan/` | `VulkanRenderer` (streaming, far LOD, frame loop), `VoxelResources` (GPU buffers) |
| `src/voxel/` | `World`/`Chunk`/`VoxelTypes`/`VoxelConfig`, `SdfField`/`SdfBox`/`SdfHandover`/`SdfUniform` |
| `src/terrain/` | `Noise(2D/3D)`, `TerrainGenerator` (3D density terrain), `FarField` (far LOD + seam patch) |
| `src/render/` | scene UBO + push constants, lighting config, image decode |
| `resources/shaders/` | `voxels.comp` — the whole renderer, one compute shader |
| `tests/` | CPU tests (no window, no Vulkan device), `ctest --test-dir build` |
| `third_party/` | vendored GLFW 3.5.1 + glm 1.0.1 + stb_image — see `third_party/README.md` |
| `scripts/build-linux-toolchain.sh` | sandbox toolchain (see below) |

## Working rules (owner-set)

- One problem per pass; passes gated on owner verification.
- No TAA (removed pass 9 after owner decision). No SSAA (removed pass 10).
  One ray per pixel.
- User uploads never reach the sandbox — diagnose from code/math/probes.
- Mouse locked always; cross-platform abstraction stays.
- Uncapped fps default; `--present fifo` restores vsync.
- Keep agent docs/toolchain inside the repo or home — `/tmp` gets wiped
  and the workspace gets rolled back occasionally (recovery: `git fetch
  origin <branch>` + `git reset --soft` + re-commit).

- **Docs discipline (pass 60, owner):** `README.md` is user-facing only — build,
  run, controls, options, troubleshooting. Shipped-feature reports go to
  `docs/PASSES.md`; agent working notes go here. No per-pass narrative, no probe
  tables, no internals in the README.

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

## Switches (command line, pass 61)

Parsed once in `main()` from `argv` by `src/core/CommandLine.cpp` into one
`vv::core::GameOptions` (`src/core/CommandLine.hpp`), read by the consumers
through `vv::core::options()`. Tests: `testCommandLine`. `game --help` prints
this list; the startup log prints what is not at its default.

| Flag | Effect |
| --- | --- |
| `--sdf-shadows` | the 3D voxel SDF soft-shadow experiment (see `docs/PASSES.md`, passes 37-57). Off = the exact binary sun march, the default reference, which must stay bit-identical |
| `--shadow-jitter <slope>` | cone slope of the per-pixel shadow-ray jitter; default `0.002` (the owner's on-device pick), `0` = off and bit-identical, clamped to 0.5; the contact floor is 10x the slope, capped at one voxel |
| `--sdf-margin <chunks>` | how far the camera may drift before the SDF box rebuilds; default 1, `0` = every chunk crossing, clamped to the box coverage |
| `--shadow-sharp` | force the exact binary shadows even with `--sdf-shadows` |
| `--far-lod` | opt into the coarse far-LOD terrain field (off by default) |
| `--validation` | enable the Khronos validation layer (debug runs: `run_debug.bat --validation`) |
| `--perf` | log frames over 25 ms, and each SDF bake's worker/render split |
| `--platform <auto\|x11\|wayland\|null\|cocoa\|win32>` | force the window platform; `null` = headless smoke run. `auto` is the default |
| `--present <immediate\|mailbox\|fifo>` | `immediate` (uncapped) is the default; `fifo` restores vsync. `uncapped` is accepted as a spelling of `immediate` |
| `--debug-term` | colour miss pixels by the ray's termination cause |
| `--debug-hole <x,z>` | far-miss diagnostics over chunk (x,z): magenta = empty far cell, cyan = data present but the ray passed above it, yellow = the march never crossed the chunk |
| `--no-<flag>` | clears a boolean flag (last one on the command line wins) |
| `-h`, `--help` | usage on stdout, exit 0 |

The historical `VV_*` environment variables still work as a fallback - they seed
the struct and a flag overrides the variable with the same meaning (scripts, CI
and the owner's on-device sweeps set them). The only `getenv` left in `src/` is
in `CommandLine.cpp`; everything else reads the parsed struct, which is what the
tests pin (including the renderer's `pc.camera.w` writer - the pass-51 failure
mode moved one indirection out, it did not go away).

`GLFW_PLATFORM` (GLFW's own variable) still works for forcing a backend; our
`--platform` covers the same ground through `glfwInitHint`, so prefer it.

## Sandbox validation

```sh
export PATH=/home/user/.cache/vv-deps/venv/bin:/home/user/.cache/vv-deps/prefix/bin:$PATH
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/home/user/.cache/vv-deps/prefix -DVV_GLFW_NULL_ONLY=ON
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=/home/user/.cache/vv-deps/prefix -DVV_GLFW_NULL_ONLY=ON
cmake --build build/release && cmake --build build/debug   # warning-free
ctest --test-dir build/release                            # all tests must pass
glslangValidator -V resources/shaders/voxels.comp -o /tmp/p.spv
LD_LIBRARY_PATH=/home/user/.cache/vv-deps/prefix/lib \
    timeout 8 ./build/release/bin/game --platform null     # headless smoke: clear message, no crash
```

`-DVV_GLFW_NULL_ONLY=ON` is for this box only (no X11/Wayland headers). The
toolchain is built by `bash scripts/build-linux-toolchain.sh`, which installs
into `~/.cache/vv-deps` (persists across the `/tmp` wipes) and needs GitHub
access; GLFW and glm are **vendored** in `third_party/`, so the script only
builds CMake/Ninja, Vulkan-Headers, Vulkan-Loader and glslang (pass 58).

CPU tests without the toolchain (pass 58 made this work with a bare `g++`:
GLFW and glm are vendored, so the test suite only needs their include dirs):

```sh
g++ -std=c++20 -O1 -ffp-contract=off -I. -Isrc \
    -I third_party/glfw/include -I third_party/glm -I third_party \
    -DGLFW_INCLUDE_NONE=1 -DVV_SHADER_DIR='"$PWD/resources/shaders"' \
    -DVV_SRC_DIR='"$PWD/src"' \
    tests/terrain_world_tests.cpp src/terrain/*.cpp src/voxel/*.cpp \
    src/render/ImageDecode.cpp src/core/InputBindings.cpp -o /tmp/t && /tmp/t
```

**Snapshot note:** `build/` and `~/.cache/` are *not* restored with the
workspace (the platform snapshots the tree, not those directories), so after a
session restore the toolchain is gone - rebuild it with
`scripts/build-linux-toolchain.sh`, or use the recipe above, which needs nothing
but `g++`.

## Where the tree stands (pass 61)

- Passes 39-58 are verified on-device. Pass 54 (one-voxel march step cap) and
  pass 55 (direction jitter) were rejected and are not in the tree; pass 56's
  origin-jitter mechanism and pass 57's distance-flat rule are the shipped
  shadow work, with the default at the owner's own pick (`--shadow-jitter
  0.002`).
  Pass 59 (the shader rename) is the only one still waiting for its launch check.
- SDF soft shadows remain an **experiment** behind `--sdf-shadows`. The exact
  binary sun march is the reference and must stay bit-identical.
- The SDF bake runs on a background thread (~57 ms of worker time on the owner's
  terrain, pass 53) and the box follows the camera within `--sdf-margin` chunks.
- Dead ends, measured — do not re-propose without new evidence: Aaltonen
  triangulation, the footprint/epsilon threshold, both minimum-penumbra forms,
  the post-loop penumbra floor, h-averaging, the pass-54 step cap, the pass-55
  fixed-cell direction jitter, P4's sliding rebuild. Details in `docs/PASSES.md`.
- **Switches are command-line flags since pass 61** (`--sdf-shadows`,
  `--shadow-jitter`, …); the `VV_*` variables remain as a fallback. See the
  table above.
- Next feature work, proposed and not scheduled: the ambient-light upgrade and
  the GPU SDF bake, both in `docs/UPGRADE_PROPOSALS.md`.
