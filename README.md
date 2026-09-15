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
and the title bar usable) and un-maximizes to **half the monitor**. Both sizes
are printed at startup (`[vv] window: ...`).

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
the softness). The field lives in two halves and the box uniform says which one
to read, so the copy never blocks a frame and a box only goes live together
with the seeds it describes; a launch that a running build swallows is retried
until the field covers the camera's chunk. Only the box is
field-aware: a shadow ray that leaves it hands over to the 2.5D penumbra
traversal (keeping the distance already marched and the visibility so far), so
casters outside the box still shadow the frame; until the first box lands the
same 2.5D traversal runs from the surface. Exact binary sun shadows remain the default
reference; `VV_SHADOW_SHARP=1` explicitly selects them.

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
