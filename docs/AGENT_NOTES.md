# Agent Notes — Vulkanic Voxels

Notes for AI agent sessions (Arena.ai Agent Mode) working in this repository.
**This chat is not persistent — read this file first.** It records what was
done, how to verify work in the sandbox, and the project's roadmap. Keep it
updated when you finish a pass.

## Project overview

Voxel rendering experiment: a single 64×64×64 voxel chunk ray-traced in a
Vulkan **compute shader** (one thread per pixel, `resources/shaders/pixels_rgba.comp`),
presented through a Qt 6 Widgets shell. C++23, CMake ≥ 3.26.

Code map:

| Path | Contents |
| --- | --- |
| `src/ui/` | Qt shell (`AppWindow`, `VulkanWidget` — hosts swapchain + game loop) |
| `src/platform/` | **Platform abstraction layer** (see below) — keep OS headers out of everything else |
| `src/vulkan/` | Renderer, buffers, swapchain, device/surface management |
| `src/voxel/` | World/chunk/voxel data model (currently a single randomly-filled chunk) |
| `src/render/` | Scene uniforms, lighting config, shader data layouts |
| `src/core/` | Camera, timer, runtime paths, file loading |
| `resources/shaders/` | GLSL compute shaders, compiled to SPIR-V at build time by glslang |
| `scripts/` | `build-linux-toolchain.sh` — sandbox build toolchain (see below) |
| `cmake/` | Build functions (GameTarget, Shaders, Packaging, …) |

### Platform abstraction layer (`src/platform/`)

Flow: `VulkanWidget` → `resolveNativeWindow()` (fills `NativeWindow`, a
Qt/Vulkan-free struct of opaque handles) → `VulkanRenderer` →
`requiredVulkanInstanceExtensions()` + `createVulkanSurface()`.

- Backends: Win32, XCB (X11), Wayland, MoltenVK (macOS). On Linux both XCB and
  Wayland are compiled in when headers exist (`VV_HAVE_XCB` / `VV_HAVE_WAYLAND`,
  decided in `cmake/GameTarget.cmake`) and chosen at run time from the QPA
  platform name.
- All WSI entry points are resolved via `vkGetInstanceProcAddr`, never linked
  directly.
- Qt private headers (`Qt6::GuiPrivate` / the QPA native interface) are
  **optional** (`VV_HAVE_QT_QPA`). Without them: Windows/macOS/X11 use public
  APIs (`QWidget::winId()`, `QNativeInterface::QX11Application`); the native
  Wayland backend is unavailable (error message points to XWayland:
  `QT_QPA_PLATFORM=xcb`). Reason: many Qt distributions (e.g. some MSYS2/MinGW
  setups) ship no private dev files.
- To add a new backend: extend `NativeWindowKind`, `NativeWindow`, the resolver
  (`QtNativeWindowResolver.cpp`), and `VulkanSurfaceFactory.cpp`. Nothing else
  should need touching.

### Input model (as of pass 1)

Mouse is locked to window center (hidden cursor) while playing. `Esc` releases
the pointer and pauses the camera; `Esc` or a click re-locks. Focus loss always
releases the pointer and clears key state. Mouse grab goes through
`QWindow::setMouseGrabEnabled` and is deferred to the first Expose event if the
window is not yet visible (this is what removed the old
`setMouseGrabEnabled: Not setting mouse grab for invisible window` warning).

## Roadmap status

**Pass 1 — done (commit "Cross-platform platform layer…"):**
- [x] Linux/macOS support via the platform abstraction layer (no unconditional
      `windows.h` anywhere)
- [x] Scraped the Qt escape/pause menu (native in-engine UI comes later);
      Escape now toggles pointer lock + pause
- [x] Fixed the mouse-grab warning (deferred grab, see above)
- [x] README with per-OS build instructions

**Pass 2 — planned, user-verified order (not started):**
- First-person always-locked mouse — *already implemented in pass 1*
- Voxel types (grass, dirt, stone…) instead of random colors
- Terrain generation with noise (open choice: hand-rolled SIMD noise vs. a
  library pulled from the web)
- Optimizations: SIMD, bitwise ops, faster traversal for large worlds
- Infinite worlds via chunking
- Infinite render distance via level of detail

## Sandbox facts (Arena.ai environment)

- **`/tmp` is ephemeral between prompts.** Anything placed there (including the
  whole Linux toolchain) can vanish between messages. Only the workspace
  (`/home/user/Vulkanic-Voxels`) persists, via snapshots — and snapshot-capped
  directories (`build/`, `.cache/`, `node_modules/`, …) do not persist either.
  Don't rely on a previous prompt's build tree existing; re-run the toolchain
  script and rebuild when in doubt.
- **Network:** only `github.com` / `codeload.github.com` / `api.github.com` /
  `pypi.org` are reachable. `apt` and Debian mirrors are blocked,
  freedesktop.org is unreachable (no real xcb/wayland headers downloadable —
  the toolchain script installs ABI-identical compile-validation shims
  instead, clearly marked inside the files).
- **No GPU / no display server.** Never expect the game to actually render in
  the sandbox; validate that it *builds, links, and fails gracefully* at run
  time.

### Verifying a Linux build in the sandbox

```sh
# One-time (or after /tmp was wiped), ~20-30 min on 2 cores:
scripts/build-linux-toolchain.sh /tmp/deps

export PATH="/tmp/deps/venv/bin:/tmp/deps/prefix/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake --build build            # must be warning-free
cmake --build build --target package_folder   # packaging check

# Headless smoke test: no GPU -> the game must show the graceful
# Vulkan error dialog (modal, so it runs until timeout) instead of crashing.
QT_QPA_PLATFORM=offscreen \
LD_LIBRARY_PATH="/tmp/deps/qt6/lib:/tmp/deps/prefix/lib" \
    timeout 5 ./build/bin/game
```

The script builds: pip CMake 3.31 + Ninja, zlib, minimal Qt 6.8.3
(offscreen/minimal QPA only — no xcb/OpenGL in Qt itself), Vulkan headers +
loader (WSI off; WSI functions resolved at run time), glm, glslang, and the
xcb/wayland compile shims. It is idempotent — re-running skips finished parts.

### Session conventions

- Work happens on the session branch (`arena/…`, shown in the environment);
  never touch other branches. Commit per pass; push with
  `git push origin <session-branch>`.
- `git push` / `gh` can fail with an authentication error when the session
  token expires (observed after ~1h). Retry on a later turn; if it persists,
  ask the user to reconnect GitHub in Arena settings.
- Code style: `.clang-format` is Google-based, **tabs**, 2-width. New UI/core
  files follow it; the older `vulkan/` and `render/` files use 2-space indent
  (don't mass-reformat, match the file you edit).

### Known caveats

- The sandbox Qt is built without xcb, so the resolver's public X11 fallback
  (`QNativeInterface::QX11Application`) cannot be compile-tested there beyond a
  manual `-DQT_FEATURE_xcb=1` object-file check; real X11 runs happen on user
  machines.
- Wayland rendering into a QWidget is best-effort (needs Qt private headers);
  X11 is the battle-tested path.
- macOS and Windows runtime behavior is verified by the user, not the sandbox.
