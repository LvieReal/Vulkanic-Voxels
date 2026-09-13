#!/bin/sh
# =============================================================================
# build-linux-toolchain.sh - self-contained Linux toolchain for building and
# verifying this project in restricted environments (no apt, no distro Qt).
#
# Motivation: the Arena.ai agent sandbox only allows HTTPS to github.com and
# pypi.org; Debian mirrors, freedesktop.org etc. are unreachable, so the usual
# `apt install qt6-base-dev libvulkan-dev ...` does not work there. This
# script builds everything the project needs from GitHub tarballs:
#
#   - CMake 3.31 + Ninja          (pip wheels, into $PREFIX/venv)
#   - zlib 1.3.1                  (Qt's -system zlib)
#   - Qt 6.8.3 (qtbase, minimal)  (offscreen/minimal QPA only: no xcb, no
#                                  OpenGL, no Vulkan in Qt itself - the game
#                                  only needs Widgets to *compile and link*)
#   - Vulkan-Headers 1.4.357      (headers only)
#   - Vulkan-Loader 1.4.357       (all WSI off; WSI entry points are resolved
#                                  through vkGetInstanceProcAddr at run time)
#   - glm 1.0.1                   (header-only)
#   - glslang 16.5.0              (glslangValidator for the shader pipeline)
#   - minimal xcb/wayland headers (ABI-identical opaque typedefs, for COMPILE
#                                  VALIDATION only - see note at the bottom)
#
# Usage:   scripts/build-linux-toolchain.sh [prefix-dir]
# Default prefix: ~/.cache/vv-deps (PERSISTS across sandbox /tmp resets,
# which wiped the toolchain 8+ times; /tmp/deps is created as a SYMLINK to
# it so the documented /tmp/deps paths keep working). Takes roughly 20-30
# minutes on 2 cores.
#
# Afterwards, configure the game:
#   export PATH="$PREFIX/venv/bin:$PREFIX/prefix/bin:$PATH"
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_PREFIX_PATH="$PREFIX/qt6;$PREFIX/prefix"
#   cmake --build build
# =============================================================================
set -eu

PREFIX="${1:-$HOME/.cache/vv-deps}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
SRC="$PREFIX/src"
BUILD="$PREFIX/build"
QT_PREFIX="$PREFIX/qt6"
DEP_PREFIX="$PREFIX/prefix"

say() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$SRC" "$BUILD" "$DEP_PREFIX"
# Compatibility symlink so /tmp/deps paths keep working after a /tmp wipe.
if [ "$PREFIX" != "/tmp/deps" ]; then
	rm -f /tmp/deps
	ln -s "$PREFIX" /tmp/deps
fi

# --- preflight ---------------------------------------------------------------
command -v curl >/dev/null || die "curl is required"
command -v g++ >/dev/null || die "g++ is required"
command -v python3 >/dev/null || die "python3 is required"
command -v perl >/dev/null || die "perl is required (Qt build)"
curl -sI --max-time 10 -o /dev/null https://codeload.github.com/ || \
	die "cannot reach codeload.github.com - this script needs GitHub access"

fetch() { # fetch <output> <owner/repo> <tag>
	if [ ! -s "$SRC/$1" ]; then
		say "Downloading $2 @ $3"
		curl -sL --fail --retry 3 -o "$SRC/$1.tmp" \
			"https://codeload.github.com/$2/tar.gz/refs/tags/$3"
		mv "$SRC/$1.tmp" "$SRC/$1"
	fi
}

# --- CMake + Ninja (pinned: 3.31.x, avoids CMake 4 policy churn) --------------
if [ ! -x "$PREFIX/venv/bin/cmake" ]; then
	say "Installing CMake + Ninja into $PREFIX/venv"
	python3 -m venv "$PREFIX/venv"
	"$PREFIX/venv/bin/pip" install -q "cmake==3.31.6" ninja
fi
export PATH="$PREFIX/venv/bin:$PATH"

# pkg-config stub: the Vulkan-Loader CMakeLists hard-requires PkgConfig on
# Linux even with every WSI backend disabled. The stub only answers
# --version; with all BUILD_WSI_*_SUPPORT=OFF no module is ever queried.
if [ ! -x "$PREFIX/venv/bin/pkg-config" ]; then
	say "Installing pkg-config stub (see comment in this script)"
	cat > "$PREFIX/venv/bin/pkg-config" <<'EOF'
#!/bin/sh
if [ "$1" = "--version" ]; then echo "1.8.0"; exit 0; fi
exit 1
EOF
	chmod +x "$PREFIX/venv/bin/pkg-config"
fi

# --- zlib (Qt's -system zlib) -------------------------------------------------
fetch zlib.tar.gz madler/zlib v1.3.1
if [ ! -f "$DEP_PREFIX/include/zlib.h" ]; then
	say "Building zlib 1.3.1"
	[ -d "$SRC/zlib-1.3.1" ] || tar xzf "$SRC/zlib.tar.gz" -C "$SRC"
	cmake -S "$SRC/zlib-1.3.1" -B "$BUILD/zlib" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEP_PREFIX" \
		-DBUILD_SHARED_LIBS=ON > /dev/null
	cmake --build "$BUILD/zlib" > /dev/null && cmake --install "$BUILD/zlib" > /dev/null
fi

# --- Qt 6.8.3 (minimal qtbase: Core, Gui, Widgets) -----------------------------
fetch qtbase.tar.gz qt/qtbase v6.8.3
if [ ! -f "$QT_PREFIX/lib/libQt6Widgets.so" ]; then
	say "Building qtbase 6.8.3 (minimal, ~20 min) - offscreen QPA only"
	[ -d "$SRC/qtbase-6.8.3" ] || tar xzf "$SRC/qtbase.tar.gz" -C "$SRC"
	cmake -S "$SRC/qtbase-6.8.3" -B "$BUILD/qtbase" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$QT_PREFIX" \
		-DCMAKE_PREFIX_PATH="$DEP_PREFIX" \
		-DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DQT_BUILD_TESTS=OFF \
		-DINPUT_freetype=qt -DINPUT_harfbuzz=qt -DINPUT_pcre=qt \
		-DINPUT_doubleconversion=qt -DINPUT_libpng=qt -DINPUT_libjpeg=qt \
		-DINPUT_opengl=no \
		-DQT_FEATURE_opengl=OFF -DQT_FEATURE_vulkan=OFF -DQT_FEATURE_dbus=OFF \
		-DQT_FEATURE_glib=OFF -DQT_FEATURE_icu=OFF -DQT_FEATURE_xcb=OFF \
		-DQT_FEATURE_egl=OFF -DQT_FEATURE_fontconfig=OFF \
		> /dev/null
	cmake --build "$BUILD/qtbase" > /dev/null
	cmake --install "$BUILD/qtbase" > /dev/null
fi

# --- Vulkan headers + loader ----------------------------------------------------
fetch vulkan-headers.tar.gz KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.357.0
if [ ! -f "$DEP_PREFIX/include/vulkan/vulkan.h" ]; then
	say "Installing Vulkan-Headers 1.4.357"
	[ -d "$SRC/Vulkan-Headers-vulkan-sdk-1.4.357.0" ] || \
		tar xzf "$SRC/vulkan-headers.tar.gz" -C "$SRC"
	cmake -S "$SRC/Vulkan-Headers-vulkan-sdk-1.4.357.0" -B "$BUILD/vulkan-headers" \
		-G Ninja -DCMAKE_INSTALL_PREFIX="$DEP_PREFIX" > /dev/null
	cmake --build "$BUILD/vulkan-headers" > /dev/null && \
		cmake --install "$BUILD/vulkan-headers" > /dev/null
fi

fetch vulkan-loader.tar.gz KhronosGroup/Vulkan-Loader vulkan-sdk-1.4.357.0
if [ ! -f "$DEP_PREFIX/lib/libvulkan.so" ]; then
	say "Building Vulkan-Loader 1.4.357 (WSI disabled)"
	[ -d "$SRC/Vulkan-Loader-vulkan-sdk-1.4.357.0" ] || \
		tar xzf "$SRC/vulkan-loader.tar.gz" -C "$SRC"
	cmake -S "$SRC/Vulkan-Loader-vulkan-sdk-1.4.357.0" -B "$BUILD/vulkan-loader" \
		-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEP_PREFIX" \
		-DCMAKE_PREFIX_PATH="$DEP_PREFIX" \
		-DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
		-DBUILD_WSI_WAYLAND_SUPPORT=OFF > /dev/null
	cmake --build "$BUILD/vulkan-loader" > /dev/null && \
		cmake --install "$BUILD/vulkan-loader" > /dev/null
fi

# --- glm (header-only) -----------------------------------------------------------
fetch glm.tar.gz g-truc/glm 1.0.1
if [ ! -f "$DEP_PREFIX/include/glm/glm.hpp" ]; then
	say "Installing glm 1.0.1"
	[ -d "$SRC/glm-1.0.1" ] || tar xzf "$SRC/glm.tar.gz" -C "$SRC"
	cp -r "$SRC/glm-1.0.1/glm" "$DEP_PREFIX/include/"
fi

# --- glslang (shader compiler) -----------------------------------------------------
fetch glslang.tar.gz KhronosGroup/glslang 16.5.0
if [ ! -x "$DEP_PREFIX/bin/glslangValidator" ]; then
	say "Building glslang 16.5.0"
	[ -d "$SRC/glslang-16.5.0" ] || tar xzf "$SRC/glslang.tar.gz" -C "$SRC"
	cmake -S "$SRC/glslang-16.5.0" -B "$BUILD/glslang" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEP_PREFIX" \
		-DENABLE_OPT=OFF -DBUILD_SHARED_LIBS=OFF > /dev/null
	cmake --build "$BUILD/glslang" > /dev/null && \
		cmake --install "$BUILD/glslang" > /dev/null
fi

# --- minimal xcb / wayland headers (COMPILE VALIDATION ONLY) ----------------------
# The real headers live on freedesktop.org / in distro packages, which are not
# reachable from the restricted environment this script was written for. The
# game only needs the ABI-stable opaque types (xcb_connection_t, xcb_window_t,
# wl_display, wl_surface) to compile the WSI surface code - the Vulkan WSI
# entry points take them as opaque pointers and are resolved at run time.
# On a real system install libxcb1-dev + libwayland-dev instead; if those
# headers already exist, this section is skipped.
if [ ! -f "$DEP_PREFIX/include/xcb/xcb.h" ]; then
	say "Installing MINIMAL xcb.h shim (compile validation only - read the warning inside)"
	mkdir -p "$DEP_PREFIX/include/xcb"
	cat > "$DEP_PREFIX/include/xcb/xcb.h" <<'EOF'
/*
 * MINIMAL SHIM HEADER - compile validation only.
 * Declares exactly the ABI-stable types consumed from
 * <vulkan/vulkan_xcb.h>. Real builds use the system libxcb headers
 * (libxcb1-dev / libxcb-devel). DO NOT build a real application against
 * this file.
 */
#ifndef XCB_SANDBOX_MINIMAL_XCB_H
#define XCB_SANDBOX_MINIMAL_XCB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_timestamp_t;
typedef uint32_t xcb_keycode_t;
typedef uint32_t xcb_button_t;

#ifdef __cplusplus
}
#endif

#endif /* XCB_SANDBOX_MINIMAL_XCB_H */
EOF
fi

if [ ! -f "$DEP_PREFIX/include/wayland-client-core.h" ]; then
	say "Installing MINIMAL wayland-client-core.h shim (compile validation only - read the warning inside)"
	cat > "$DEP_PREFIX/include/wayland-client-core.h" <<'EOF'
/*
 * MINIMAL SHIM HEADER - compile validation only.
 * Declares the ABI-stable opaque types from the real header; the Vulkan
 * WSI entry points only consume the wl_display and wl_surface handles as
 * opaque pointers. Real builds use the system Wayland headers
 * (libwayland-dev / wayland-devel). DO NOT build a real application
 * against this file.
 */
#ifndef WAYLAND_CLIENT_CORE_SANDBOX_MINIMAL_H
#define WAYLAND_CLIENT_CORE_SANDBOX_MINIMAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_event_queue;
struct wl_proxy;
struct wl_interface;
struct wl_surface;
struct wl_callback;
struct wl_compositor;

struct wl_display *wl_display_connect(const char *name);
void wl_display_disconnect(struct wl_display *display);

#ifdef __cplusplus
}
#endif

#endif /* WAYLAND_CLIENT_CORE_SANDBOX_MINIMAL_H */
EOF
fi

# --- done ------------------------------------------------------------------------
say "Toolchain ready under $PREFIX"
cat <<EOF

Configure and build the game:

  export PATH="$PREFIX/venv/bin:$PREFIX/prefix/bin:\$PATH"
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_PREFIX_PATH="$QT_PREFIX;$DEP_PREFIX"
  cmake --build build

Headless smoke test (no GPU/display in restricted sandboxes - the game must
fail gracefully with the Vulkan error dialog, not crash):

  QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$QT_PREFIX/lib:$DEP_PREFIX/lib" \\
      timeout 5 ./build/bin/game
EOF
