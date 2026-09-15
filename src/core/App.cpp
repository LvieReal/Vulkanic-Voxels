#include "core/App.hpp"

#include <algorithm>
#include <cstdio>

#include "core/Version.h"
#include "platform/GlfwNativeWindow.hpp"
#include "vulkan/VulkanRenderer.hpp"

namespace vv::core {

namespace {

const char* layoutKeyName(int key, int scancode) {
	(void)scancode;
	return glfwGetKeyName(key, 0);
}

}  // namespace

void App::RendererDeleter::operator()(
		vv::vulkan::VulkanRenderer* renderer) const {
	delete renderer;
}

App::~App() = default;

bool App::init(std::string& outError) {
	GameWindow::Hooks hooks{};
	hooks.user = this;
	hooks.onFramebufferSize = [](void* user, std::uint32_t width,
	                             std::uint32_t height) {
		static_cast<App*>(user)->syncRendererSize(width, height);
	};
	hooks.onKey = [](void* user, int key, int scancode, int action, int mods) {
		static_cast<App*>(user)->handleKey(key, scancode, action, mods);
	};
	hooks.onCursorPos = [](void* user, double x, double y) {
		static_cast<App*>(user)->handleCursorPosition(x, y);
	};
	hooks.onMouseButton = [](void* user, int button, int action, int mods) {
		static_cast<App*>(user)->handleMouseButton(button, action, mods);
	};
	// Scrolling is not bound to anything yet; the callback is wired so the
	// event does not reach the platform default (which could zoom a future
	// in-engine UI).
	hooks.onScroll = [](void*, double, double) {};
	hooks.onFocusLost = [](void* user) {
		static_cast<App*>(user)->handleFocusLost();
	};

	if (!m_window.init(hooks, outError)) {
		return false;
	}

	// The window manager answers the maximize request asynchronously, so wait
	// for the window's size to settle BEFORE the swapchain is created for it
	// (pass 45: creating it for the restore size and fixing it on the next
	// resize callback left a stretched launch frame).
	m_window.settleFramebufferSize();

	vv::platform::NativeWindow native =
	    vv::platform::resolveNativeWindow(m_window.handle(), outError);
	if (!native.isValid()) {
		outError = "Failed to obtain a native window handle: " + outError;
		return false;
	}
	std::fprintf(stderr, "[vv] window backend: %s\n", [&native]() {
		switch (native.kind) {
			case vv::platform::NativeWindowKind::Win32:
				return "Win32";
			case vv::platform::NativeWindowKind::X11:
				return "X11";
			case vv::platform::NativeWindowKind::Wayland:
				return "Wayland";
			case vv::platform::NativeWindowKind::Cocoa:
				return "Cocoa";
			case vv::platform::NativeWindowKind::Unknown:
				break;
		}
		return "unknown";
	}());

	m_bindings = defaultBindings();
	logBindings(m_bindings, &layoutKeyName);

	m_renderer.reset(new vv::vulkan::VulkanRenderer());
	m_renderer->setWorldConfig(m_voxelConfig);

	vv::vulkan::VulkanRenderer::InitInfo init{};
	init.nativeWindow = native;
	init.width = m_window.framebufferWidth();
	init.height = m_window.framebufferHeight();
	if (!m_renderer->init(init, outError)) {
		outError = "Vulkan init failed: " + outError;
		m_renderer.reset();
		return false;
	}
	m_rendererWidth = init.width;
	m_rendererHeight = init.height;
	// Both numbers in the log: a swapchain that does not match the window is a
	// rendering report's first suspect, and this makes it visible immediately.
	std::fprintf(stderr, "[vv] swapchain: %ux%u (window reports %ux%u)\n",
	             m_renderer->swapchainWidth(), m_renderer->swapchainHeight(),
	             m_window.framebufferWidth(), m_window.framebufferHeight());

	// Spawn above the terrain at the center of chunk (0,0), looking out over
	// the world.
	m_camera.setFovDegrees(70.0f);
	m_camera.setMoveSpeed(40.0f);
	m_camera.setPosition(m_renderer->spawnPosition());
	m_camera.setYawPitchDegrees(180.0f, -18.0f);
	m_gameTimer.reset();

	m_initialized = true;
	m_mouseLocked = true;
	m_window.setMouseLocked(true);

	// Ground truth for remote debugging: name the exact binary and the live
	// render state (also mirrored into the window title, refreshed 1 Hz).
	std::fprintf(stderr, "[vv] build=%s | %s\n", vv::core::kBuildId,
	             m_renderer->debugStats().c_str());
	m_lastStatsSeconds = m_gameTimer.totalSeconds();
	refreshDebugTitle();
	return true;
}

void App::setMouseLocked(bool locked) {
	if (locked == m_mouseLocked) {
		return;
	}
	m_mouseLocked = locked;
	m_window.setMouseLocked(locked);
	if (!locked) {
		resetActionStates();
	}
	m_haveLastCursor = false;
}

void App::syncRendererSize(std::uint32_t width, std::uint32_t height) {
	if (m_renderer == nullptr || width == 0 || height == 0) {
		// Before the renderer exists the window's size is read again for its
		// creation (see init), so there is nothing to chase here.
		return;
	}
	if (width == m_rendererWidth && height == m_rendererHeight) {
		return;
	}
	std::fprintf(stderr, "[vv] swapchain: %ux%u -> %ux%u\n", m_rendererWidth,
	             m_rendererHeight, width, height);
	m_rendererWidth = width;
	m_rendererHeight = height;
	m_renderer->resize(width, height);
	// Report the size the driver actually gave us: a swapchain that does not
	// match the window is the first thing to look at in a render report.
	std::fprintf(stderr, "[vv] swapchain: now %ux%u (window %ux%u)\n",
	             m_renderer->swapchainWidth(), m_renderer->swapchainHeight(),
	             width, height);
}

void App::handleFocusLost() {
	// The window dropped the lock (alt-tab, OS shortcut): never keep the
	// pointer confined or the camera running with stale key state.
	if (m_mouseLocked) {
		m_mouseLocked = false;
		m_window.setMouseLocked(false);
	}
	resetActionStates();
	setGamePaused(true);
}

void App::handleKey(int key, int scancode, int action, int mods) {
	(void)mods;

	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
		if (m_mouseLocked) {
			setMouseLocked(false);
			setGamePaused(true);
		} else {
			setMouseLocked(true);
			setGamePaused(false);
		}
		return;
	}

	// Only the press/release edges move the camera; auto-repeat is ignored,
	// so a held key's repeat does not toggle anything.
	if (action != GLFW_PRESS && action != GLFW_RELEASE) {
		return;
	}
	const Action bound = lookupAction(m_bindings, key, scancode);
	if (bound != Action::Count) {
		setActionState(bound, action == GLFW_PRESS);
	}
}

void App::handleMouseButton(int button, int action, int mods) {
	(void)mods;
	// Clicking the view resumes mouse lock (e.g. after Escape or a focus
	// loss) instead of dragging a "view" around.
	if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS &&
			!m_mouseLocked) {
		setMouseLocked(true);
		setGamePaused(false);
	}
}

void App::handleCursorPosition(double x, double y) {
	if (!m_mouseLocked) {
		m_haveLastCursor = false;
		return;
	}
	if (!m_haveLastCursor) {
		// First sample after a lock: remember it, do not treat it as motion.
		// (GLFW_CURSOR_DISABLED keeps reporting the window centre, but the
		// very first sample can be the position the cursor had before.)
		m_lastCursorX = x;
		m_lastCursorY = y;
		m_haveLastCursor = true;
		return;
	}
	const double dx = x - m_lastCursorX;
	const double dy = y - m_lastCursorY;
	m_lastCursorX = x;
	m_lastCursorY = y;
	if (dx == 0.0 && dy == 0.0) {
		return;
	}
	m_camera.addMouseDeltaPixels(static_cast<float>(dx), static_cast<float>(dy));
}

void App::setActionState(Action action, bool down) {
	const std::size_t index = static_cast<std::size_t>(action);
	if (index < static_cast<std::size_t>(Action::Count)) {
		m_actionDown[index] = down;
	}
}

void App::resetActionStates() {
	for (bool& down : m_actionDown) {
		down = false;
	}
}

void App::setGamePaused(bool paused) { m_gameTimer.setPaused(paused); }

void App::tick() {
	if (m_renderer == nullptr) {
		return;
	}

	// Fatal GPU error: surface it once and stop rendering (the loop keeps
	// the window responsive so the message stays readable).
	if (m_renderer->deviceLost()) {
		if (!m_deviceLostReported) {
			m_deviceLostReported = true;
			setMouseLocked(false);
			setGamePaused(true);
			std::fprintf(stderr,
			             "[vv] fatal: the GPU or driver reported an error and "
			             "rendering was stopped:\n%s\n"
			             "If this mentions a timeout, the compute workload was "
			             "too heavy for the GPU; lowering the render radius or "
			             "trace steps in VoxelConfig helps.\n",
			             m_renderer->lastError().c_str());
		}
		return;
	}

	const float dt = std::min(0.050f, m_gameTimer.tickSeconds());

	glm::vec3 moveLocal(0.0f);
	if (m_actionDown[static_cast<std::size_t>(Action::MoveLeft)]) {
		moveLocal.x -= 1.0f;
	}
	if (m_actionDown[static_cast<std::size_t>(Action::MoveRight)]) {
		moveLocal.x += 1.0f;
	}
	if (m_actionDown[static_cast<std::size_t>(Action::MoveDown)]) {
		moveLocal.y -= 1.0f;
	}
	if (m_actionDown[static_cast<std::size_t>(Action::MoveUp)]) {
		moveLocal.y += 1.0f;
	}
	if (m_actionDown[static_cast<std::size_t>(Action::MoveForward)]) {
		moveLocal.z += 1.0f;
	}
	if (m_actionDown[static_cast<std::size_t>(Action::MoveBack)]) {
		moveLocal.z -= 1.0f;
	}

	const float speedMul =
	    m_actionDown[static_cast<std::size_t>(Action::SpeedBoost)] ? 3.0f
	                                                               : 1.0f;
	m_camera.moveLocal(moveLocal, dt, speedMul);

	m_renderer->setCamera(m_camera,
	                      static_cast<float>(m_gameTimer.totalSeconds()));

	// Keep the GPU chunk region centered on the camera (no-op unless the
	// camera crossed a chunk boundary).
	m_renderer->updateWorld(m_camera.position());

	// Do not present while the window is not on screen (minimized/hidden);
	// presenting to an unexposed surface just burns swapchain cycles.
	if (!m_window.canPresent()) {
		return;
	}
	// The framebuffer-size callback is the fast path; this is the guarantee.
	// A window manager is free to change the size without the callback having
	// been delivered before the next frame (and a missed callback used to
	// leave the swapchain on the pre-maximize size).
	syncRendererSize(m_window.framebufferWidth(), m_window.framebufferHeight());
	m_renderer->drawFrame();

	// 1 Hz debug title refresh (build id + live state + fps).
	++m_statFrames;
	const double now = m_gameTimer.totalSeconds();
	if (now - m_lastStatsSeconds >= 1.0) {
		m_titleFps = static_cast<double>(m_statFrames) /
								 std::max(1e-9, now - m_lastStatsSeconds);
		m_lastStatsSeconds = now;
		m_statFrames = 0;
		refreshDebugTitle();
	}
}

void App::refreshDebugTitle() {
	if (m_renderer == nullptr) {
		return;
	}
	char title[512] = {};
	std::snprintf(title, sizeof(title),
	              "Vulkanic Voxels - build %s | %s | %.0f fps",
	              vv::core::kBuildId, m_renderer->debugStats().c_str(),
	              m_titleFps);
	m_window.setTitle(title);
}

int App::run() {
	if (!m_initialized || m_renderer == nullptr) {
		std::fprintf(stderr, "[vv] fatal: run() before a successful init().\n");
		return 1;
	}

	while (!m_window.shouldClose()) {
		// Block for events instead of spinning: with v-sync off the frame
		// loop would otherwise saturate a core and starve the compositor.
		m_window.waitEvents();
		if (m_window.shouldClose()) {
			break;
		}
		if (m_window.minimized()) {
			continue;
		}
		if (m_renderer->deviceLost()) {
			// Keep the (empty) window alive so the stderr message above can
			// be read and the user closes it normally.
			continue;
		}
		tick();
	}
	return 0;
}

}  // namespace vv::core
