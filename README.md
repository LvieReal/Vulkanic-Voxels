# Vulkanic Voxels

A voxel-tech experiment: infinite terrain ray-traced in a Vulkan **compute
shader** (one thread per pixel), presented through Qt Widgets.

The world is an infinite (X/Z) grid of chunks, each 32×128×32 voxels generated
from a deterministic Perlin-style fBm heightmap. Voxels are typed materials
(grass, dirt, stone, sand, snow, bedrock — grass has distinct top/side/bottom
colors), stored as one byte per voxel packed 4-per-uint32 in a GPU chunk
atlas. A region of chunks around the camera stays resident on the GPU
(re-centered when you cross a chunk boundary), and distance fog hides the
region edge.

The renderer lives in `src/vulkan`, the world/voxel model in `src/voxel`,
terrain generation in `src/terrain`, and the Qt shell in `src/ui`. Everything
platform-specific (Win32/X11/Wayland/macOS windowing) is isolated behind
`src/platform` so the renderer never touches an OS header.

## Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move |
| `Space` / `Ctrl` | Fly up / down |
| `Shift` | Sprint (×3) |
| Mouse | Look (pointer locked to window center, no button needed) |
| `Esc` | Release pointer & pause camera — press again (or click) to resume |

## Building

The project uses CMake (≥ 3.26), C++23, Qt 6 (Core, Gui, Widgets) and the
Vulkan SDK (headers + loader). Shaders are compiled at build time by
`glslangValidator`.

### Windows (MSYS2 / MinGW-w64)

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-vulkan-headers \
          mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-glslang
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.exe
```

### Linux

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake qt6-base-dev libvulkan-dev \
                 glslang-tools libglm-dev libxcb1-dev libwayland-dev
```

Fedora:

```sh
sudo dnf install gcc-c++ cmake qt6-qtbase-devel vulkan-headers \
                 vulkan-loader-devel glslang-devel glm-devel \
                 libxcb-devel wayland-devel
```

> **Optional:** Qt's private headers (`qt6-base-private-dev` on Debian/Ubuntu)
> enable the native **Wayland** backend. The game builds and runs without them
> on Windows, macOS and X11; on Wayland without them, run via XWayland:
> `QT_QPA_PLATFORM=xcb ./build/release/bin/game`.

Build and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game
```

A small pure-logic test suite (noise, terrain layering, chunked world) builds
alongside by default — run it with `ctest --test-dir build` (or disable with
`-DVV_BUILD_TESTS=OFF`).

### Render experiments

The full-detail terrain region is the default. Set `VV_FAR_LOD=1` to enable
the optional coarse terrain field beyond it; without that variable LOD is off,
so no far-field build or seam ring is generated. Set `VV_SDF_SHADOWS=1` to try
the SDF soft-shadow marcher: the exact shadow ray's traversal with an
extended-sun-disk (circle-segment) penumbra estimate - the fraction of the sun
disk above each column's top plane, continuous at the top, so every heightfield
shadow edge gets a soft penumbra (`kShadowSharpness` in the shader tunes the
sun's angular size / softness). Exact binary sun shadows remain the default
reference; `VV_SHADOW_SHARP=1` explicitly selects them.

Both the X11 (XCB) and Wayland surface backends are compiled in when their
headers are found; the correct one is picked at run time from the Qt platform.

### macOS

Requires a Vulkan implementation with [MoltenVK]
(VK_MVK_macos_surface) — e.g. the [Vulkan SDK] for macOS.

```sh
brew install cmake qt vulkan-sdk glslang
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.app/Contents/MacOS/game   # or use the build/package_folder target
```

### Packaging

`cmake --build build --target package_folder` produces a distributable folder
in `build/dist/<config>` with the Qt runtime deployed next to the executable.

## Troubleshooting

- **Wayland glitches / black window**: run under XWayland with
  `QT_QPA_PLATFORM=xcb ./build/release/bin/game`. Rendering into a Qt widget on
  Wayland is best-effort; X11 is the battle-tested path.
- **"Required Vulkan instance extension 'VK_KHR_*_surface' is not supported"**:
  your Vulkan loader is too old or misconfigured. Update your GPU drivers /
  Vulkan runtime.
- **"The X11 (XCB) Vulkan surface backend was not compiled in"**: install the
  xcb/Wayland development headers (see above) and delete the build directory
  so CMake re-runs its checks.

[Vulkan SDK]: https://vulkan.lunarg.com/sdk/home
[MoltenVK]: https://github.com/KhronosGroup/MoltenVK
