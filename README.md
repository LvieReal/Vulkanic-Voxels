# Vulkanic Voxels

A voxel-tech experiment: infinite terrain ray-traced in a Vulkan **compute
shader** (one thread per pixel), presented through GLFW.

The world is an infinite (X/Z) grid of chunks, each 32×128×32 voxels generated
from a deterministic Perlin-style fBm heightmap. Voxels are typed materials
(grass, dirt, stone, sand, snow, bedrock — grass has distinct top/side/bottom
colors), stored as one byte per voxel packed 4-per-uint32 in a GPU chunk
atlas. A region of chunks around the camera stays resident on the GPU
(re-centered when you cross a chunk boundary), and distance fog hides the
region edge.

The renderer lives in `src/vulkan`, the world/voxel model in `src/voxel`,
terrain generation in `src/terrain`, and the window/input/frame loop in
`src/core` (`App`, `GameWindow`, `InputBindings`). Everything platform-specific
(Win32/X11/Wayland/macOS windowing) is isolated behind `src/platform`, so the
renderer never touches a windowing header: GLFW owns the window, the Vulkan
surface is created from the native handles GLFW exposes, and the one vendored
library (`stb_image`, see `third_party/README.md`) decodes the optional texture
PNGs.

Movement keys are bound **by physical position** (the Linux evdev / Windows
Set-1 / macOS virtual codes), so `W` `A` `S` `D` stay in the same place on
AZERTY, QWERTZ and Dvorak keyboards; the layout key is accepted as well, and the
startup log prints which position and which label each action got.

## Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` (physical position) | Move |
| `Space` / `Ctrl` | Fly up / down |
| `Shift` | Sprint (×3) |
| Mouse | Look (cursor disabled while playing, relative motion, no button needed) |
| `Esc` | Release the cursor & pause the camera — press again (or click) to resume |
| Window | Starts maximized; un-maximizing (title-bar button or WM shortcut) gives a window half the monitor size |

## Building

The project uses CMake (≥ 3.26), C++23, [GLFW] and the Vulkan SDK (headers +
loader). Shaders are compiled at build time by `glslangValidator`.

GLFW is picked up from the system when a CMake package for it exists
(`libglfw3-dev`, `mingw-w64-ucrt-x86_64-glfw`, `brew install glfw`); otherwise
CMake fetches the pinned release once and builds it with the game, so the
version is the same everywhere. Configure with `-DVV_GLFW_NULL_ONLY=ON` to
build GLFW's headerless null backend instead - that is how restricted
environments (no X11/Wayland development packages) get a compiling,
window-less build; see `scripts/build-linux-toolchain.sh`.

### Windows (MSYS2 / MinGW-w64)

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-glfw mingw-w64-ucrt-x86_64-vulkan-headers \
          mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-glslang
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.exe
```

### Linux

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libglfw3-dev libvulkan-dev \
                 glslang-tools libglm-dev libwayland-dev libxkbcommon-dev
```

(GLFW's X11 backend needs the X11/XRandR/Xinerama/Xcursor/XInput/XKB
development packages; `libglfw3-dev` pulls them in. Without a system GLFW,
CMake fetches one and needs the same headers.)

Fedora:

```sh
sudo dnf install gcc-c++ cmake glfw-devel vulkan-headers \
                 vulkan-loader-devel glslang-devel glm-devel \
                 libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel \
                 libXi-devel libXkbcommon-devel wayland-devel
```

> Both the X11 and Wayland backends are compiled in when GLFW was built with
> them, and GLFW picks the right one at run time (it follows the session, and
> `GLFW_PLATFORM` / `VV_PLATFORM=null` can override it). There is no XWayland
> requirement and no private toolkit headers involved.

Build and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game
```

The window opens **maximized** (decorated, so the window manager keeps panels
and the title bar usable) and un-maximizes to **half the monitor**. It is sized
and centred first, still hidden, and only then maximized - so the window manager
places the maximized window itself instead of the app moving an already
maximized one. Both sizes are printed at startup (`[vv] window: ...`), plus one
line with the framebuffer size, whether the window reports itself maximized and
where its content sits, and the size the swapchain was created for
(`[vv] swapchain: ...`). Before the swapchain is created the app waits for the
window manager to answer the maximize request, and it keeps re-checking the
window size on every frame, so the first frame is already the size the window
really has (no stretched or mis-sized launch frame on X11 or Wayland, with or
without an explicit resize afterwards).

Minimizing the window does not touch the swapchain: a minimized window has no
drawable size (Win32 reports `0x0`), so the rebuild is deferred until the window
is back and the size the surface reports is never handed to the driver as-is
(`[vulkan] swapchain rebuild deferred: the surface reports no size (window
minimized)`).

Debug runs enable the Khronos **validation layer**: `run_debug.bat` sets
`VV_VALIDATION=1` before starting `build/debug/bin/game.exe` (any other run can
do the same: `VV_VALIDATION=1 ./build/release/bin/game`). Validation messages
arrive on stderr with the `[vulkan]` prefix; when the layer is not installed the
app says so and continues without it.

A small pure-logic test suite (noise, terrain layering, chunked world, key
bindings, the PNG decoder) builds alongside by default — run it with
`ctest --test-dir build` (or disable with `-DVV_BUILD_TESTS=OFF`).

### Render experiments

The full-detail terrain region is the default. Set `VV_FAR_LOD=1` to enable
the optional coarse terrain field beyond it; without that variable LOD is off,
so no far-field build or seam ring is generated. Set `VV_SDF_SHADOWS=1` to try
the SDF soft-shadow marcher: a 3D voxel SDF of the near terrain (a
camera-centered 6x6-chunk box over the full world height, kept on the camera's
chunk by a background build) is sphere-traced toward the sun with the plain
Quilez `k*h/t` penumbra estimate, so shadow edges are soft on vertical / side
casters too and not just on flat tops (`kShadowSharpness` in the shader tunes
the softness, and `VV_SHADOW_JITTER` tunes the per-point ray jitter below). The field lives in two halves and the box uniform says where the live
half starts, so the copy never blocks a frame and a box only goes live together
with the seeds it describes. Only the box is
field-aware: a shadow ray that leaves it hands over to the 2.5D penumbra
traversal (keeping the distance already marched and the visibility so far), so
casters outside the box still shadow the frame; until the first box lands the
same 2.5D traversal runs from the surface.

The seeds themselves are packed so the shader can decode them cheaply (pass
50): each cell's nearest-solid cell is stored as three bit fields
(`x | y << bits.x | z << (bits.x + bits.y)`, the widths picked per box - 23 bits
for the default terrain) and decoded with two shifts and two masks. The old
layout was a linear cell index, which cost three integer divisions per fetch;
integer division is a long instruction sequence on a GPU, and the shader used
to run that decode up to 27 times per sphere-trace step, so it showed up as the
hottest code in an Nsight capture. The decoded cell is identical - this is a
pure arithmetic change, nothing about the field or the picture moves.
The widths travel to the shader in the box uniform, written word-for-word
through one layout definition (`vv::voxel::SdfBoxUniform`,
`src/voxel/SdfUniform.hpp`) that the writer and the tests both go through, so a
word of the block cannot go missing unnoticed (pass 51: pass 50 added the
`seedBits` words and never stored them - zero masks, every gather degenerate,
and the 3D shadows silently disappeared). Copying the compiled SPIR-V next to
the executable is a build step as well, not a side effect of linking, so a
shader-only rebuild always replaces the `.spv` the game actually loads.

A sphere-trace step no longer gathers the 3x3x3 block around the sample (pass
52). The field is cell-centred, so the candidates that can carry the distance
are the eight cells whose centres surround the point (`floor(p - 0.5)` and
`+1`, clamped into the box - clamping is the same as skipping a corner,
because a duplicate cannot change a min), and they are compared as squared
distances, so a step pays eight fetches and one `sqrt` instead of up to 27
fetches and eight. A CPU probe marched the shipping field with both gathers:
174 taps per ray against 593, with the step count, the blocked share and the
mean visibility unchanged (0.0003 mean / 0.075 max visibility difference on the
rays that graze a cave mouth). This is what the P3 idea (a quantized per-cell
distance field) was for - the tap target - and it reaches it with no new
buffer and no new upload. The distance field itself was measured too and left
out: its trilinear read lightens the soft shadows (mean visibility 0.695 ->
0.858, ~1.4% of rays changing verdict) because the 3x3x3 min's over-report near
corners is what keeps the current penumbra dark, and no bias brings that back.

Pass 57 jitters the shadow ray per pixel as a CONE plus a fixed world
displacement, so the noise is the same at every camera distance. The ray's
direction is tilted by a hashed slope inside a cone (the sun's LENGTH is kept,
because the march's t is a distance), and the origin slides along the surface by
a fixed `kShadowJitterFloor` voxels, never by a footprint. It answers the two
defects the owner found on-device in pass 56, whose per-pixel ORIGIN jitter he
had verified ("looks correct now"): its strength, where "0.5 is too small, 5 is
enough to eliminate banding and stepping", and - the real find - its distance
dependence, "noise scales with distance, so up close it's still not enough, and
too far it's too much". That second one was pass 56's rule itself: the offset was
scaled by the shaded point's PIXEL FOOTPRINT, the one quantity in the shader that
grows with the camera, and the estimate moves by k*offset/t, t being the
distance to the caster. On the 3721-ray terrain probe the mean |dvis| ran 0.008
-> 0.167 (22x) over camera distances 5 -> 400; the same rays under a cone hold
0.021, flat. A cone can never move a contact (t ~= 0 there, the pass-55 finding),
so the origin keeps its own displacement - a fixed world distance, in the surface
tangent plane so a displaced origin cannot start inside the solid it stands on -
and the two share one hashed azimuth, so it stays one jittered ray. The hash cell
is still the pixel's footprint, pass 56's accepted per-pixel grain. At the shipped
default (cone slope 0.05, floor 0.50 vox, reached at the renderer's `VV_SHADOW_JITTER`
lever; 0 is bit-identical to the un-jittered estimate, the clamp is 0.5 and the
startup log prints both numbers) the contact scan's grid-locked zigzag - the
"stair-stepping" - drops from 0.162 to 0.063 voxel while its profile becomes
per-sample grain (worst neighbour step 0.037 -> 0.957), the terrains' 18.2% of
rays move by more than 1/255, and the mean visibility of the probe's terrain view
goes 0.6556 -> 0.7122: the edge folds toward the light by half a voxel, the same
trade pass 56 made at the offset the owner called "enough". Costs two hashes per
shadow ray against ~174 field taps; the march's samples and the exact binary path
(`VV_SDF_SHADOWS=0`) are untouched, bit for bit.

Pass 56 jitters the ray's ORIGIN, one pixel of world, in the surface tangent
plane - superseded by pass 57's rule for the reason above, but the mechanism it
introduced (a per-pixel displacement hashed in footprint-sized cells, which is
what "jitter per pixel" turned out to mean) is what pass 57 kept. Pass 55 tilted
the sun's DIRECTION instead and the owner's verdict pinpointed the mechanism: the
noise arrived "in small squares" (a fixed 1/8-voxel hash cell is several pixels
wide at range), 0.01 was still too high, and "at contacts (small penumbra)
there's no jitter at all".
Pass 54's one-voxel step cap (`kMaxSdfStep`) was rejected on-device ("looks
like it did not solve anything, only performs worse now") and is reverted. Its
probe numbers still stand as measurements - the edge profile's summed second
difference dropped 0.29 -> 0.13 and its worst neighbour step 0.037 -> 0.017 for
0.0004 mean visibility of terrain change - but they did not read on screen,
and the field phase really did go 21.9 -> 77.3 of its 160 samples per ray. Ray
jitter is what replaced it.

The rebake's own arithmetic got cheaper in pass 53: the two chamfer sweeps now
scan a cell's seven candidates in one go and write the cell once, instead of
seven relaxation calls that each loaded, compared and stored the cell - the same
candidates in the same order with the same strict comparison, so the field is
bit-identical (a test freezes the old sequential form and requires every seed
word and every distance to match, on a shape set and on real banded terrain).
Measured on the shipping box: the two sweeps 121 -> 29 ms, the worker's whole
bake ~62 ms instead of ~165 ms on the same machine, with the uploaded array
unchanged. The sliding rebuild that was planned for this pass was measured first
and dropped: shifting the live field and repairing it locally cannot reproduce
the two-pass field (52 of 4 055 040 cells at shipping size, and worse if the
repair is iterated to convergence - the shipped two-pass chamfer is not
converged), and it saved only 12% anyway.

Two things keep the rebake cheap (pass 49). The box's empty sky is cropped: the
build keeps the highest solid cell of its footprint plus 16 voxels of penumbra
margin, so a bake is 110 of the 128 rows on the default terrain (14% fewer
cells, 16.2 MB instead of 18.9 MB to upload) - the retained field is cell-for-
cell identical to the full-height one, and the shader needs no special case,
because a ray crossing the new top face hands over exactly as it did through
`y = worldHeight`. And the field is recentered only when it has to be: the box
covers +/-3 chunks, so the camera may drift `VV_SDF_MARGIN` chunks (default 1,
`0` restores the pre-49 "every chunk crossing" cadence, `2` is the most the
coverage allows) from the live field's center before a rebuild is armed, and
the rebuild aims at the camera's *current* chunk rather than the one the stream
finished. A rebuild used to be requested once per completed region move (every
~32 voxels of travel), which kept the background builder running a rebake loop
behind a moving camera; `VV_PERF=1` now reports each bake
(`[perf] SDF bake #N: 192x110x192 of 128 cells (16.2 MB seeds) at chunk (x,z) |
worker: band + build ms | render: snapshot + upload ms | s since the previous
bake`), so the remaining cost is measurable instead of guessed at. Exact binary
sun shadows remain the default reference; `VV_SHADOW_SHARP=1` explicitly selects
them.

Both the X11 (Xlib) and Wayland surface backends are compiled in when GLFW
has them; the correct one is picked at run time from the platform GLFW reports.
A headless run (`VV_PLATFORM=null`, requires a GLFW without a real backend)
creates the window object and fails with a clear message instead of a crash -
useful as a smoke test.

### macOS

Requires a Vulkan implementation with [MoltenVK]
(VK_MVK_macos_surface) — e.g. the [Vulkan SDK] for macOS.

```sh
brew install cmake glfw vulkan-sdk glslang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.app/Contents/MacOS/game   # or use the build/package_folder target
```

### Packaging

`cmake --build build --target package_folder` produces a distributable folder
in `build/dist/<config>`. There is no toolkit runtime to deploy any more: the
executable, the SPIR-V shaders and the optional `resources/textures` tree are
the whole package.

## Troubleshooting

- **Wayland glitches / black window**: force X11 with
  `GLFW_PLATFORM=x11 ./build/release/bin/game` (XWayland). X11 remains the
  battle-tested path; GLFW picks the session's backend otherwise.
- **"Required Vulkan instance extension 'VK_KHR_*_surface' is not supported"**:
  your Vulkan loader is too old or misconfigured. Update your GPU drivers /
  Vulkan runtime.
- **"The X11 Vulkan surface backend was not compiled in"**: GLFW has no X11
  backend in this build; install the X11 development packages (see above) and
  delete the build directory so CMake re-runs its checks.
- **"glfwInit failed"** with "This binary only supports the Null platform": the
  GLFW library has no windowing backend (a `-DVV_GLFW_NULL_ONLY=ON` build).
  Configure without that option on a machine with a display server.
- **Wrong movement keys**: the startup log prints one line per action
  (`[vv] key move forward: key 87 (w), scancode 17`), including the position
  and the label the windowing layer reported - that is the ground truth for
  "the keyboard behaves differently here" reports.

[GLFW]: https://www.glfw.org/
[Vulkan SDK]: https://vulkan.lunarg.com/sdk/home
[MoltenVK]: https://github.com/KhronosGroup/MoltenVK
