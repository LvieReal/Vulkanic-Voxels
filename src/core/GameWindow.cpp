#include "core/GameWindow.hpp"

#include "core/CommandLine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vv::core {

namespace {

// GLFW is refcounted; the app owns one init/terminate pair.
bool g_glfwInitialized = false;

void glfwErrorCallback(int code, const char* description) {
	std::fprintf(stderr, "[glfw] error %d: %s\n", code,
							 description != nullptr ? description : "(no description)");
}

// Where the window's content should be recentered (physical pixels -> screen
// coordinates through the current window position and the content scale).
void cursorCenter(GLFWwindow* window, double* outX, double* outY) {
	int windowX = 0;
	int windowY = 0;
	int width = 0;
	int height = 0;
	glfwGetWindowPos(window, &windowX, &windowY);
	glfwGetWindowSize(window, &width, &height);
	float scaleX = 1.0f;
	float scaleY = 1.0f;
	glfwGetWindowContentScale(window, &scaleX, &scaleY);
	*outX = static_cast<double>(windowX) +
					static_cast<double>(width) * 0.5 * static_cast<double>(scaleX);
	*outY = static_cast<double>(windowY) +
					static_cast<double>(height) * 0.5 * static_cast<double>(scaleY);
}

}  // namespace

GameWindow::~GameWindow() {
	if (m_window != nullptr) {
		glfwDestroyWindow(m_window);
		m_window = nullptr;
	}
	if (g_glfwInitialized) {
		glfwTerminate();
		g_glfwInitialized = false;
	}
}

bool GameWindow::init(const Hooks& hooks, std::string& outError) {
	m_hooks = hooks;

	glfwSetErrorCallback(glfwErrorCallback);

	if (!g_glfwInitialized) {
		// --platform <name> forces the window platform (the parser has already
		// rejected anything that is not one of the names). "null" is the
		// headless escape hatch for CI and sandboxes - it is the only way to get
		// GLFW's null platform, because auto-selection never picks it - and the
		// other names are the explicit override for a session where the wrong
		// backend was chosen. The hint must be set before glfwInit(); a platform
		// this GLFW was not built with fails right there with GLFW's own
		// message.
		const std::string& requested = options().platform;
		if (!requested.empty() && requested != "auto") {
			// 0 = nothing to hint: the parser only lets the six names through,
			// so this is an exhaustive table, not a fallback.
			int platform = 0;
			if (requested == "null") {
				platform = GLFW_PLATFORM_NULL;
			} else if (requested == "x11") {
				platform = GLFW_PLATFORM_X11;
			} else if (requested == "wayland") {
				platform = GLFW_PLATFORM_WAYLAND;
			} else if (requested == "cocoa") {
				platform = GLFW_PLATFORM_COCOA;
			} else if (requested == "win32") {
				platform = GLFW_PLATFORM_WIN32;
			}
			if (platform != 0) {
				glfwInitHint(GLFW_PLATFORM, platform);
			}
		}
		if (glfwInit() != GLFW_TRUE) {
			const char* description = nullptr;
			glfwGetError(&description);
			outError = "glfwInit failed";
			if (description != nullptr) {
				outError += std::string(": ") + description;
			}
			outError += ".";
			return false;
		}
		g_glfwInitialized = true;
	}
	// Failures from here on are reported by the error callback above (which
	// also names the backend); init() adds the game-specific context.

	// Vulkan owns the presentation: no client API, no double buffer.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	// The window is created hidden, sized and placed FIRST, and maximized only
	// afterwards, still hidden (pass 46). The GLFW_MAXIMIZED create hint cannot
	// be used here: on Win32 it puts WS_MAXIMIZE on the window at creation
	// (win32_window.c creates with the style, and Windows maximizes such a
	// window there and then), so the glfwSetWindowPos() below would move a
	// window that is already maximized - Windows keeps the maximized size but
	// applies the move to the maximized window, and the window comes up with
	// its top-left corner somewhere near the middle of the screen. Maximizing
	// explicitly after the placement takes each backend's pre-map path
	// instead: Win32 maximizeWindowManually() computes the work-area rect
	// itself, X11 appends _NET_WM_STATE_MAXIMIZED_{HORZ,VERT} to the unmapped
	// window, Wayland records the state for the toplevel it creates on show,
	// Cocoa zooms. So the placement stays window-manager policy and the size
	// this process asked for stays the geometry the window restores to.
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	// MAXIMIZED BY DEFAULT, with half the monitor as the size to restore to
	// (pass 44). Pass 43 sized a borderless window to the work area by hand,
	// which covered the panel/taskbar on the owner's desktop and looked like
	// fullscreen; "maximized" means work area minus panels, decorations and
	// dock autohide, and that is window-manager policy this process cannot
	// reimplement portably.
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	int areaX = 0;
	int areaY = 0;
	int areaWidth = 0;
	int areaHeight = 0;
	if (monitor != nullptr) {
		glfwGetMonitorWorkarea(monitor, &areaX, &areaY, &areaWidth, &areaHeight);
	}
	const GLFWvidmode* mode =
			monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
	const int minWidth = 640;
	const int minHeight = 400;
	// Half the monitor (the work area when there is one - a panel eats part of
	// the screen): the window the user gets back on un-maximize.
	const int halfOfWidth =
			(areaWidth > 0 ? areaWidth : (mode != nullptr ? mode->width : 1280)) /
			2;
	const int halfOfHeight =
			(areaHeight > 0 ? areaHeight : (mode != nullptr ? mode->height : 720)) /
			2;
	const int restoreWidth = std::max(minWidth, halfOfWidth);
	const int restoreHeight = std::max(minHeight, halfOfHeight);

	m_window = glfwCreateWindow(restoreWidth, restoreHeight, "Vulkanic Voxels",
															nullptr, nullptr);
	if (m_window == nullptr) {
		const char* description = nullptr;
		glfwGetError(&description);
		outError = "Failed to create the GLFW window";
		if (description != nullptr) {
			outError += std::string(": ") + description;
		}
		outError += ".";
		return false;
	}

	// Where the restore-size window goes: centred in the work area.
	const int restoreX = areaX + (areaWidth - restoreWidth) / 2;
	const int restoreY = areaY + (areaHeight - restoreHeight) / 2;

	// Centre the restored-size window in the work area. Wayland does not let a
	// client place its windows and reports the attempt as an error; the
	// compositor decides, which is fine (it will be maximized anyway).
	const int platform = glfwGetPlatform();
	if (platform != GLFW_PLATFORM_WAYLAND && areaWidth > 0 && areaHeight > 0) {
		glfwSetWindowPos(m_window, restoreX, restoreY);
	}

	glfwSetWindowSizeLimits(m_window, minWidth, minHeight, GLFW_DONT_CARE,
												 GLFW_DONT_CARE);
	glfwSetWindowUserPointer(m_window, this);

	// Maximize while the window is still hidden and after the position above:
	// the window manager places the maximized window (that rectangle is its
	// policy) and keeps the centred half-monitor rect as the geometry to
	// restore to on un-maximize.
	glfwMaximizeWindow(m_window);

	// On Win32 - the only backend whose position query is live before the
	// window is mapped - check where the maximize actually landed and re-apply
	// the sequence if it is not where a maximized window belongs (that is what
	// the user does by hand when grabbing the title bar fixes it). The other
	// backends cannot be asked here: X11 would report the position this process
	// requested, and Wayland never reports one.
	if (platform == GLFW_PLATFORM_WIN32) {
		verifyMaximizedPlacement(areaX, areaY, restoreX, restoreY);
	}

	// Shown last: the window is already maximized and placed, and the size
	// above is the geometry the window manager restores to on un-maximize.
	glfwShowWindow(m_window);

	int fbWidth = 0;
	int fbHeight = 0;
	glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
	m_framebufferWidth = static_cast<std::uint32_t>(std::max(1, fbWidth));
	m_framebufferHeight = static_cast<std::uint32_t>(std::max(1, fbHeight));

	int winWidth = 0;
	int winHeight = 0;
	glfwGetWindowSize(m_window, &winWidth, &winHeight);
	float scaleX = 1.0f;
	float scaleY = 1.0f;
	glfwGetWindowContentScale(m_window, &scaleX, &scaleY);
	std::fprintf(stderr,
						 "[vv] window: %dx%d window pixels -> %ux%u framebuffer "
						 "pixels (content scale %.2fx%.2f, platform %d)\n",
						 winWidth, winHeight, m_framebufferWidth, m_framebufferHeight,
						 static_cast<double>(scaleX), static_cast<double>(scaleY),
						 platform);
	std::fprintf(stderr,
						 "[vv] window: maximized (work area %dx%d, restores to %dx%d), "
						 "decorations on\n",
						 areaWidth, areaHeight, restoreWidth, restoreHeight);

	applyMainLoopHooks();
	return true;
}

void GameWindow::applyMainLoopHooks() {
	glfwSetFramebufferSizeCallback(m_window, &GameWindow::framebufferSizeCallback);
	glfwSetKeyCallback(m_window, &GameWindow::keyCallback);
	glfwSetCursorPosCallback(m_window, &GameWindow::cursorPosCallback);
	glfwSetMouseButtonCallback(m_window, &GameWindow::mouseButtonCallback);
	glfwSetScrollCallback(m_window, &GameWindow::scrollCallback);
	glfwSetWindowFocusCallback(m_window, &GameWindow::focusCallback);
}

bool GameWindow::shouldClose() const {
	return m_window == nullptr || glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

void GameWindow::requestClose() {
	if (m_window != nullptr) {
		glfwSetWindowShouldClose(m_window, GLFW_TRUE);
	}
}

void GameWindow::waitEvents() {
	if (m_window == nullptr) {
		return;
	}
	// One event per frame is processed here; glfwPollEvents() inside the
	// renderer's frame work (or the next call) drains the rest. Waiting keeps
	// the CPU off the spin loop when v-sync is off.
	if (glfwGetWindowAttrib(m_window, GLFW_VISIBLE) == GLFW_TRUE) {
		glfwWaitEventsTimeout(0.001);
	} else {
		glfwWaitEventsTimeout(0.05);
	}
}

void GameWindow::verifyMaximizedPlacement(int areaX, int areaY, int restoreX,
																		 int restoreY) {
	if (m_window == nullptr) {
		return;
	}
	// NOTE: GLFW's GLFW_MAXIMIZED flag is not consulted here. On Win32 it is
	// driven by WM_SIZE messages, so it is still false at this point (the
	// window has never been shown); the maximize this checks was requested
	// unconditionally one line above and, on Win32, is applied by
	// maximizeWindowManually() directly, without messaging.

	// 64 px of slack covers the caption and frame of a correctly maximized
	// window, and is far less than the half-screen offset a misplaced one has.
	constexpr int kTolerance = 64;
	int x = 0;
	int y = 0;
	glfwGetWindowPos(m_window, &x, &y);
	if (x >= areaX - kTolerance && x <= areaX + kTolerance &&
			y >= areaY - kTolerance && y <= areaY + kTolerance) {
		return;
	}

	std::fprintf(stderr,
							 "[vv] window: the maximize left the window at %d,%d instead "
							 "of the work area origin %d,%d; re-applying the placement\n",
							 x, y, areaX, areaY);
	glfwRestoreWindow(m_window);
	glfwSetWindowPos(m_window, restoreX, restoreY);
	glfwMaximizeWindow(m_window);

	int movedX = 0;
	int movedY = 0;
	glfwGetWindowPos(m_window, &movedX, &movedY);
	std::fprintf(stderr, "[vv] window: maximized placement is now %d,%d\n",
							 movedX, movedY);
}

void GameWindow::settleFramebufferSize(double maxWaitSeconds) {
	if (m_window == nullptr) {
		return;
	}
	const std::uint32_t initialWidth = m_framebufferWidth;
	const std::uint32_t initialHeight = m_framebufferHeight;

	// Each iteration blocks up to 10 ms in the event loop, so this is not a
	// spin. The size counts as settled after three identical samples AND at
	// least kStableSeconds of wall time, because events can arrive fast enough
	// for three samples to pass in well under a millisecond.
	constexpr double kStepSeconds = 0.01;
	constexpr double kStableSeconds = 0.03;
	constexpr int kStableSamples = 3;
	const auto start = std::chrono::steady_clock::now();
	double waited = 0.0;
	int stable = 0;
	std::uint32_t lastWidth = m_framebufferWidth;
	std::uint32_t lastHeight = m_framebufferHeight;
	while (waited < maxWaitSeconds) {
		glfwWaitEventsTimeout(kStepSeconds);
		const std::chrono::steady_clock::time_point sampled =
				std::chrono::steady_clock::now();
		waited = std::chrono::duration<double>(sampled - start).count();

		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(m_window, &width, &height);
		m_framebufferWidth = static_cast<std::uint32_t>(std::max(1, width));
		m_framebufferHeight = static_cast<std::uint32_t>(std::max(1, height));

		if (m_framebufferWidth == lastWidth &&
				m_framebufferHeight == lastHeight) {
			if (++stable >= kStableSamples && waited >= kStableSeconds) {
				break;
			}
		} else {
			stable = 0;
			lastWidth = m_framebufferWidth;
			lastHeight = m_framebufferHeight;
		}
	}

	if (m_framebufferWidth != initialWidth ||
			m_framebufferHeight != initialHeight) {
		std::fprintf(stderr,
								 "[vv] window: settled to %ux%u framebuffer pixels after "
								 "%d ms (the maximize request was answered by the "
								 "window manager)\n",
								 m_framebufferWidth, m_framebufferHeight,
								 static_cast<int>(waited * 1000.0 + 0.5));
	}

	// One geometry line after the window manager has had its say: the size the
	// frames use, whether the window came up maximized, and where its content
	// area sits. A report about the launch window then carries its own numbers.
	const bool maximized =
			glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;
	if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
		// Wayland does not tell a client where its window is; asking would print
		// through the GLFW error callback.
		std::fprintf(stderr,
								 "[vv] window: %ux%u framebuffer pixels, maximized %s "
								 "(Wayland does not report the position)\n",
								 m_framebufferWidth, m_framebufferHeight,
								 maximized ? "yes" : "no");
	} else {
		int contentX = 0;
		int contentY = 0;
		glfwGetWindowPos(m_window, &contentX, &contentY);
		std::fprintf(stderr,
								 "[vv] window: %ux%u framebuffer pixels, maximized %s, "
								 "content at %d,%d\n",
								 m_framebufferWidth, m_framebufferHeight,
								 maximized ? "yes" : "no", contentX, contentY);
	}
}

bool GameWindow::canPresent() const {
	if (m_window == nullptr) {
		return false;
	}
	return glfwGetWindowAttrib(m_window, GLFW_VISIBLE) == GLFW_TRUE &&
				 glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_FALSE;
}

bool GameWindow::minimized() const {
	if (m_window == nullptr) {
		return true;
	}
	return glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE;
}

void GameWindow::setMouseLocked(bool locked) {
	if (m_window == nullptr || locked == m_mouseLocked) {
		return;
	}
	m_mouseLocked = locked;
	if (locked) {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		// The cursor position is unspecified right after a disable; recentre
		// so the next relative delta does not start with a jump.
		centerCursorOnWindow();
	} else {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
}

void GameWindow::centerCursorOnWindow() {
	if (m_window == nullptr) {
		return;
	}
	double centerX = 0.0;
	double centerY = 0.0;
	cursorCenter(m_window, &centerX, &centerY);
	glfwSetCursorPos(m_window, centerX, centerY);
}

void GameWindow::setTitle(const std::string& title) {
	if (m_window != nullptr) {
		glfwSetWindowTitle(m_window, title.c_str());
	}
}

void GameWindow::framebufferSizeCallback(GLFWwindow* window, int width,
																				 int height) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self == nullptr) {
		return;
	}
	self->m_framebufferWidth = static_cast<std::uint32_t>(std::max(1, width));
	self->m_framebufferHeight = static_cast<std::uint32_t>(std::max(1, height));
	if (self->m_hooks.onFramebufferSize != nullptr) {
		self->m_hooks.onFramebufferSize(self->m_hooks.user,
																	 self->m_framebufferWidth,
																	 self->m_framebufferHeight);
	}
}

void GameWindow::keyCallback(GLFWwindow* window, int key, int scancode,
														 int action, int mods) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self != nullptr && self->m_hooks.onKey != nullptr) {
		self->m_hooks.onKey(self->m_hooks.user, key, scancode, action, mods);
	}
}

void GameWindow::cursorPosCallback(GLFWwindow* window, double x, double y) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self != nullptr && self->m_hooks.onCursorPos != nullptr) {
		self->m_hooks.onCursorPos(self->m_hooks.user, x, y);
	}
}

void GameWindow::mouseButtonCallback(GLFWwindow* window, int button, int action,
																		 int mods) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self != nullptr && self->m_hooks.onMouseButton != nullptr) {
		self->m_hooks.onMouseButton(self->m_hooks.user, button, action, mods);
	}
}

void GameWindow::scrollCallback(GLFWwindow* window, double x, double y) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self != nullptr && self->m_hooks.onScroll != nullptr) {
		self->m_hooks.onScroll(self->m_hooks.user, x, y);
	}
}

void GameWindow::focusCallback(GLFWwindow* window, int focused) {
	auto* self = static_cast<GameWindow*>(glfwGetWindowUserPointer(window));
	if (self == nullptr) {
		return;
	}
	if (focused == GLFW_FALSE) {
		// Losing focus (alt-tab, OS shortcut) must never keep the pointer
		// confined or the camera running with stale key state.
		self->setMouseLocked(false);
		if (self->m_hooks.onFocusLost != nullptr) {
			self->m_hooks.onFocusLost(self->m_hooks.user);
		}
	}
}

}  // namespace vv::core
