# Vulkanic Voxels

A voxel-tech experiment: infinite terrain ray-traced in a Vulkan **compute
shader** (one thread per pixel), presented through GLFW.

The world is an infinite (X/Z) grid of chunks, each 32×128×32 voxels carved from
a deterministic 3D Perlin-style density field - hills, mountains and overhangs
alike. Voxels are typed materials (grass, dirt, stone, sand, snow, bedrock, with
different top/side/bottom colours), stored one byte per voxel in a GPU chunk
atlas. A region of chunks around the camera stays resident on the GPU and distance
fog hides the edge of it.

Movement keys are bound **by physical position**, so `W` `A` `S` `D` stay in the
same place on AZERTY, QWERTZ and Dvorak keyboards; the layout label is accepted
as well, and the startup log prints what each action got.

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

CMake (≥ 3.26), C++23, the Vulkan SDK (headers + loader) and
`glslangValidator`. Nothing else: [GLFW] and [glm] are **vendored** in
`third_party/` (see `third_party/README.md`), so there is no `glfw`/`glm`
package to install and no download at configure time.

The only per-platform install left is what the window backend itself needs - on
Linux the X11/Wayland development packages (the vendored GLFW compiles whichever
are present, and falls back to a window-less null backend if neither is, which
is enough for the tests). `-DVV_USE_SYSTEM_DEPS=ON` prefers system `glfw3`/`glm`
when they exist (packagers); `-DVV_GLFW_NULL_ONLY=ON` asks for the null backend
on purpose (`scripts/build-linux-toolchain.sh` covers sandboxes without any
windowing headers).

### Windows (MSYS2 / MinGW-w64)

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-vulkan-headers \
          mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-glslang
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.exe
```

`run.bat` does the same from Explorer (it adds MSYS2's `mingw64\bin` to `PATH`)
and forwards any additional options; `run_debug.bat` builds nothing but starts
the debug binary with `--validation`.

### Linux

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libvulkan-dev glslang-tools
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
                 libxi-dev libxkbcommon-dev libwayland-dev   # for a window
```

Fedora:

```sh
sudo dnf install gcc-c++ cmake vulkan-headers vulkan-loader-devel glslang \
                 libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel \
                 libXi-devel libxkbcommon-devel wayland-devel
```

Then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game        # or ./run.sh
```

The window opens maximized (decorated, so the window manager keeps panels and
the title bar usable) and un-maximizes to half the monitor; the first frame is
already the size the window really has, and minimizing is handled without
touching the swapchain. Both sizes are printed at startup (`[vv] window: ...`).

Debug runs enable the Khronos **validation layer**: `run_debug.bat` passes
`--validation`, and `./build/release/bin/game --validation` does the same for any
other binary. Messages arrive on stderr; when the layer is not installed the app
says so and continues.

A small pure-logic test suite (noise, terrain layering, chunked world, key
bindings, the PNG decoder) builds alongside by default: `ctest --test-dir build`
(disable with `-DVV_BUILD_TESTS=OFF`).

### macOS

Requires a Vulkan implementation with [MoltenVK] (VK_MVK_macos_surface) — e.g.
the [Vulkan SDK] for macOS.

```sh
brew install cmake vulkan-sdk glslang
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/release/bin/game.app/Contents/MacOS/game   # or use the build/package_folder target
```

### Packaging

`cmake --build build --target package_folder` produces a distributable folder
in `build/dist/<config>`: the executable, the SPIR-V shaders and the optional
`resources/textures` tree are the whole package.

## Command line

`game --help` prints the list. Every switch also accepts a `--no-` prefix where
that makes sense (`--no-far-lod`), and the last one on the command line wins.

| Option | Effect |
| --- | --- |
| `--sdf-shadows` | try the SDF soft-shadow marcher instead of the exact binary sun shadows (the default reference) |
| `--shadow-jitter <slope>` | strength of the per-pixel shadow-ray jitter, `0` = off (default `0.002`) |
| `--sdf-margin <chunks>` | how far the camera may drift before the SDF field is rebuilt (default 1) |
| `--shadow-sharp` | force the exact binary sun shadows |
| `--far-lod` | enable the coarse far-terrain LOD field (off by default) |
| `--validation` | enable the Khronos validation layer |
| `--perf` | log slow frames and the SDF bake's cost split |
| `--present <mode>` | `immediate` (uncapped, the default), `mailbox` or `fifo` (vsync) |
| `--platform <name>` | `auto` (default), `x11`, `wayland`, `null`, `cocoa` or `win32` — force the window platform |
| `--debug-term` | colour pixels by why the ray ended |
| `--debug-hole <x,z>` | diagnostics for a far-LOD hole over chunk (`x`,`z`) |

The start-up log prints the switches that are not at their defaults
(`[vv] options: sdf-shadows, shadow-jitter 0.050, ...`), which is what a bug
report should quote. The `VV_*` environment variables of the same names still
work as a fallback for scripts and CI (a flag overrides its variable), and an
unknown flag or a malformed value stops the run with a message instead of
starting with settings you did not ask for.

## Troubleshooting

- **Wayland glitches / black window**: force X11 with
  `GLFW_PLATFORM=x11 ./build/release/bin/game` (XWayland). GLFW picks the
  session's backend otherwise.
- **"Required Vulkan instance extension 'VK_KHR_*_surface' is not supported"**:
  your Vulkan loader is too old or misconfigured. Update your GPU drivers /
  Vulkan runtime.
- **"The X11 Vulkan surface backend was not compiled in"**: GLFW has no X11
  backend in this build; install the X11 development packages (see above),
  delete the build directory and configure again (or pass
  `-DVV_USE_SYSTEM_DEPS=ON` to use a system GLFW that has it).
- **"glfwInit failed"** with "This binary only supports the Null platform": the
  GLFW library has no windowing backend - the X11/Wayland development packages
  were missing at configure time, or the build used `-DVV_GLFW_NULL_ONLY=ON`.
  Install them (see above), delete the build directory so CMake re-runs its
  checks, and configure again.
- **Wrong movement keys**: the startup log prints one line per action
  (`[vv] key move forward: key 87 (w), scancode 17`), including the position and
  the label the windowing layer reported - that is the ground truth for "the
  keyboard behaves differently here" reports.

## Documentation

| File | Contents |
| --- | --- |
| `docs/PASSES.md` | shipped-feature reports, one per pass |
| `docs/UPGRADE_PROPOSALS.md` | proposed upgrades that are not shipped (ambient light for caves, GPU SDF bake) |
| `docs/AGENT_NOTES.md` | working notes for agent sessions (contracts, switches, sandbox recipes) |
| `docs/reference_renderer.wgsl` | the WGSL reference renderer this look came from |
| `third_party/README.md` | the vendored dependencies and their exact revisions |

[GLFW]: https://www.glfw.org/
[glm]: https://github.com/g-truc/glm
[Vulkan SDK]: https://vulkan.lunarg.com/sdk/home
[MoltenVK]: https://github.com/KhronosGroup/MoltenVK
