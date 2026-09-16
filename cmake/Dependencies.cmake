# ---------------------------------------------------------------------------
# Dependencies.
#
# glfw and glm are VENDORED in third_party/ - upstream sources, trimmed to what
# the build needs (see third_party/README.md). A clone therefore builds with
# nothing installed but a C++ toolchain and the Vulkan headers/loader, and with
# no network access: there is no FetchContent and no find_package to fail.
#
# -DVV_USE_SYSTEM_DEPS=ON prefers a system glfw3/glm when one is present (a
# packager's build); everything else is the same either way.
#
# Linux: GLFW builds its X11 and Wayland backends, which need their development
# packages (libxcb/libxrandr/libxinerama/libxcursor/libxi/libxkb + libwayland-dev
# + libxkbcommon-dev). Whichever of the two is installed is what gets built and
# what this build compiles against; with neither, GLFW's headerless null backend
# is built and a warning names the packages (a restricted sandbox can also ask
# for that on purpose with -DVV_GLFW_NULL_ONLY=ON).
# ---------------------------------------------------------------------------
option(VV_USE_SYSTEM_DEPS
    "Prefer system glfw3/glm over the vendored copies in third_party/" OFF)

set(VV_GLFW_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/glfw")
set(VV_GLM_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/glm")
# The version the vendored tree is expected to be. Bumping third_party/glfw
# means updating this line in the same commit - it is a pin, not a preference.
set(VV_GLFW_EXPECTED_VERSION "3.5.1")

# --- glm (header-only) ------------------------------------------------------
# A fresh probe variable name: the pass-43 configure cached VV_GLM_INCLUDE_DIR
# as a PATH, and a stale cache entry must not shadow the vendored copy.
unset(VV_GLM_INCLUDE_DIR CACHE)
if(VV_USE_SYSTEM_DEPS)
    find_path(VV_GLM_SYSTEM_INCLUDE_DIR glm/glm.hpp)
endif()
if(VV_USE_SYSTEM_DEPS AND VV_GLM_SYSTEM_INCLUDE_DIR)
    set(VV_GLM_INCLUDE_DIR "${VV_GLM_SYSTEM_INCLUDE_DIR}")
    message(STATUS "glm: system headers in ${VV_GLM_INCLUDE_DIR}")
else()
    set(VV_GLM_INCLUDE_DIR "${VV_GLM_VENDOR_DIR}")
    if(NOT EXISTS "${VV_GLM_INCLUDE_DIR}/glm/glm.hpp")
        message(FATAL_ERROR
            "glm is neither installed nor vendored: expected "
            "${VV_GLM_VENDOR_DIR}/glm/glm.hpp")
    endif()
    message(STATUS "glm: vendored (third_party/glm, upstream 1.0.1)")
endif()

# --- which native window backends exist (Linux/BSD) -------------------------
# GLFW picks a backend at run time; these variables only say what is compiled
# and which definitions our own sources (glfw3native.h users) need.
set(VV_GLFW_HAS_X11 OFF)
set(VV_GLFW_HAS_WAYLAND OFF)
if(UNIX AND NOT APPLE AND NOT VV_GLFW_NULL_ONLY)
    find_path(VV_GLFW_X11_HEADERS X11/Xlib.h)
    find_path(VV_GLFW_WAYLAND_HEADERS wayland-client-core.h)
    if(VV_GLFW_X11_HEADERS)
        set(VV_GLFW_HAS_X11 ON)
    endif()
    if(VV_GLFW_WAYLAND_HEADERS)
        set(VV_GLFW_HAS_WAYLAND ON)
    endif()
endif()

# --- glfw -------------------------------------------------------------------
if(VV_USE_SYSTEM_DEPS)
    find_package(glfw3 3.3 QUIET)
endif()
if(glfw3_FOUND)
    set(VV_GLFW_SOURCE "system")
    message(STATUS "glfw: system package ${glfw3_VERSION}")
else()
    set(VV_GLFW_SOURCE "vendored")
    if(NOT EXISTS "${VV_GLFW_VENDOR_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "GLFW is neither installed nor vendored: expected "
            "${VV_GLFW_VENDOR_DIR}/CMakeLists.txt")
    endif()
    file(READ "${VV_GLFW_VENDOR_DIR}/include/GLFW/glfw3.h" _vv_glfw_header
        LIMIT 40000)
    foreach(_part MAJOR MINOR REVISION)
        string(REGEX MATCH "GLFW_VERSION_${_part}[ \t]+([0-9]+)"
            _vv_glfw_match "${_vv_glfw_header}")
        set(_vv_glfw_${_part} "${CMAKE_MATCH_1}")
    endforeach()
    set(_vv_glfw_version
        "${_vv_glfw_MAJOR}.${_vv_glfw_MINOR}.${_vv_glfw_REVISION}")
    if(NOT _vv_glfw_version STREQUAL VV_GLFW_EXPECTED_VERSION)
        message(FATAL_ERROR
            "third_party/glfw reports GLFW ${_vv_glfw_version}, but this build "
            "pins ${VV_GLFW_EXPECTED_VERSION}. Update third_party/glfw and "
            "VV_GLFW_EXPECTED_VERSION in cmake/Dependencies.cmake together.")
    endif()
    message(STATUS "glfw: vendored ${_vv_glfw_version} (third_party/glfw)")

    # Built as a subproject of this build: no install step, no fetch.
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ${VV_GLFW_HAS_X11} CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ${VV_GLFW_HAS_WAYLAND} CACHE BOOL "" FORCE)
    add_subdirectory("${VV_GLFW_VENDOR_DIR}"
        "${CMAKE_BINARY_DIR}/third_party/glfw" EXCLUDE_FROM_ALL)
endif()
set(VV_GLFW_TARGET glfw)

if(UNIX AND NOT APPLE AND NOT VV_GLFW_NULL_ONLY
        AND NOT VV_GLFW_HAS_X11 AND NOT VV_GLFW_HAS_WAYLAND)
    message(WARNING
        "Neither the X11 nor the Wayland development packages are installed, "
        "so GLFW is built with its null backend and the game cannot open a "
        "window on this system. Install them (libxcb/libxrandr/libxinerama/"
        "libxcursor/libxi/libxkb + libwayland-dev + libxkbcommon-dev on "
        "Debian/Ubuntu) and reconfigure.")
endif()

find_package(Vulkan REQUIRED)

# GLFW and the background build threads need the threading library.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
