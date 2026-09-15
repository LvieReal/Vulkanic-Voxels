# GLFW is the windowing layer (pass 43 replaced Qt with it). A system install
# is used when present; otherwise the pinned release is fetched from GitHub
# and built as a subproject - one small static library instead of a whole
# toolkit, the same version on every machine.
#
# Linux: GLFW builds its X11 and Wayland backends by default and needs their
# development packages (libxcb/libxrandr/libxinerama/libxcursor/libxi/libxkb
# and libwayland-dev + libxkbcommon-dev). Configure with
# -DVV_GLFW_NULL_ONLY=ON to build GLFW's headerless null backend instead (the
# restricted sandbox uses that for compile validation).
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    message(STATUS "GLFW not found on the system: fetching glfw 3.5.1")
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    if(VV_GLFW_NULL_ONLY)
        # Restricted environments without X11/Wayland dev headers.
        set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.5.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
endif()
set(VV_GLFW_TARGET glfw)

find_package(Vulkan REQUIRED)

# GLFW and the background build threads need the threading library.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# glm is header-only; locate it explicitly so the game target (and the test
# suite) always has it, regardless of which other package drags it in.
find_path(VV_GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)
