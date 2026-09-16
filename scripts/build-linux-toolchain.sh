#!/bin/sh
# =============================================================================
# build-linux-toolchain.sh - self-contained Linux toolchain for building and
# verifying this project in restricted environments (no apt, no windowing
# development packages).
#
# Motivation: the Arena.ai agent sandbox only allows HTTPS to github.com and
# pypi.org; Debian mirrors, freedesktop.org etc. are unreachable, so the usual
# `apt install libvulkan-dev glslang-tools ...` does not work there. This
# script builds everything the project needs from GitHub tarballs:
#
#   - CMake 3.31 + Ninja          (pip wheels, into $PREFIX/venv)
#   - Vulkan-Headers 1.4.357      (headers only)
#   - Vulkan-Loader 1.4.357       (all WSI off; WSI entry points are resolved
#                                  through vkGetInstanceProcAddr at run time)
#   - glslang 16.5.0              (glslangValidator for the shader pipeline)
#
# GLFW and glm are NOT built here any more: pass 58 vendored them into
# third_party/, so they arrive with the checkout. The game has needed no
# windowing toolkit since pass 43, so this script is a few minutes instead of
# half an hour - and with the vendored pair it needs neither their headers nor
# a display to produce a build that walks the game's headless path end to
# end.
#
# Usage:   scripts/build-linux-toolchain.sh [prefix-dir]
# Default prefix: ~/.cache/vv-deps (PERSISTS across sandbox /tmp resets,
# which wiped the toolchain 8+ times; /tmp/deps is created as a SYMLINK to
# it so the documented /tmp/deps paths keep working).
#
# Afterwards, configure the game:
#   export PATH="$PREFIX/venv/bin:$PREFIX/prefix/bin:$PATH"
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_PREFIX_PATH="$PREFIX/prefix" -DVV_GLFW_NULL_ONLY=ON
#   cmake --build build
#
# On a real Linux box (X11/Wayland development headers available) drop
# -DVV_GLFW_NULL_ONLY=ON; the vendored GLFW then builds the backends whose
# development packages are installed.
# =============================================================================
set -eu

PREFIX="${1:-$HOME/.cache/vv-deps}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
SRC="$PREFIX/src"
BUILD="$PREFIX/build"
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

# --- done ------------------------------------------------------------------------
say "Toolchain ready under $PREFIX"
cat <<EOF

Configure and build the game:

  export PATH="$PREFIX/venv/bin:$PREFIX/prefix/bin:\$PATH"
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_PREFIX_PATH="$PREFIX/prefix" -DVV_GLFW_NULL_ONLY=ON
  cmake --build build

Headless smoke test (no GPU/display in restricted sandboxes - the game must
create its window, resolve the platform and fail with a clear message
instead of crashing):

  VV_PLATFORM=null LD_LIBRARY_PATH="$PREFIX/prefix/lib" \\
      timeout 5 ./build/release/bin/game
EOF
