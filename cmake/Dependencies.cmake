find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Vulkan REQUIRED)

# glm is header-only; locate it explicitly so the game target (and the test
# suite) always has it, regardless of which other package drags it in.
find_path(VV_GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)

# Note: the game also uses Qt's private QtGui headers (the QPA native
# interface) when available - see GameTarget.cmake. Without them the game
# still builds and runs on Windows, macOS and X11; only the native Wayland
# backend is lost (run via XWayland: QT_QPA_PLATFORM=xcb).
