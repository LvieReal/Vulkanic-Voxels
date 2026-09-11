find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Vulkan REQUIRED)

# Note: the game also links Qt6::GuiPrivate (see GameTarget.cmake) for Qt's
# installed private QtGui headers (the QPA native interface used to query the
# native window handles for Vulkan surface creation). The Qt6::GuiPrivate
# target is created by the regular Qt6 Gui package; the headers themselves are
# shipped in qt6-base-private-dev (Debian/Ubuntu), qt6-qtbase-devel (Fedora),
# and are included in brew's qt and MSYS2's qt6-base packages.
