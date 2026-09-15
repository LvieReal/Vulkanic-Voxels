#include "core/GameWindow.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
		// Headless escape hatch (CI, sandboxes): VV_PLATFORM=null selects
		// GLFW's null platform explicitly. It is the only way to get one when
		// the GLFW library has no real backend compiled in, and it needs the
		// hint because auto-selection never picks "null".
		if (const char* requested = std::getenv("VV_PLATFORM")) {
			if (std::strcmp(requested, "null") == 0) {
				glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
			} else {
				std::fprintf(stderr,
										 "[vv] warning: VV_PLATFORM=%s is not a known value "
										 "(only 'null' is understood)\n",
										 requested);
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

	// The game ran "maximized" until this pass; a borderless window on the
	// primary monitor's work area is the closest GLFW equivalent. On a
	// compositor that cannot do undecorated windows the decorated fallback is
	// used (the same path a failed borderless creation takes).
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	int areaX = 0;
	int areaY = 0;
	int areaWidth = 1280;
	int areaHeight = 720;
	if (monitor != nullptr) {
		glfwGetMonitorWorkarea(monitor, &areaX, &areaY, &areaWidth, &areaHeight);
	}
	int scaleWidth = areaWidth;
	int scaleHeight = areaHeight;

	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	m_window = glfwCreateWindow(areaWidth, areaHeight, "Vulkanic Voxels",
															nullptr, nullptr);
	if (m_window == nullptr) {
		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
		m_window = glfwCreateWindow(scaleWidth, scaleHeight, "Vulkanic Voxels",
																nullptr, nullptr);
	}
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

	glfwSetWindowPos(m_window, areaX, areaY);
	glfwSetWindowSizeLimits(m_window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
	glfwSetWindowUserPointer(m_window, this);

	const int platform = glfwGetPlatform();
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
							 winWidth, winHeight, m_framebufferWidth,
							 m_framebufferHeight, static_cast<double>(scaleX),
							 static_cast<double>(scaleY), platform);

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
