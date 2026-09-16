#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Camera.hpp"
#include "core/GameTimer.hpp"
#include "core/GameWindow.hpp"
#include "core/InputBindings.hpp"
#include "voxel/VoxelConfig.hpp"

namespace vv::vulkan {
class VulkanRenderer;
}

namespace vv::core {

// The game: owns the window, the renderer, the camera, the frame loop and the
// input state. Replaces the toolkit-era arrangement (removed in pass 43): a
// window object, a render widget and a frame timer were three objects doing
// what one loop does here.
//
// Input model (unchanged): the mouse is locked to the window while playing
// and the camera follows relative motion; Escape releases it and pauses;
// Escape or a click re-locks. Keys are matched against the bindings from
// InputBindings (physical scancode where the platform has one, label key as
// the fallback channel), not against hard-coded key codes.
class App final {
 public:
	App() = default;
	~App();

	App(const App&) = delete;
	App& operator=(const App&) = delete;

	// Creates the window and the renderer. On failure outError carries the
	// reason (stderr-friendly, no dialog); the caller exits non-zero.
	bool init(std::string& outError);

	// Runs the frame loop until the window closes. Returns the process exit
	// code (0 for a normal close, 2 for a fatal GPU error).
	int run();

	bool mouseLocked() const { return m_mouseLocked; }
	void setMouseLocked(bool locked);

 private:
	void handleKey(int key, int scancode, int action, int mods);
	void handleMouseButton(int button, int action, int mods);
	void handleCursorPosition(double x, double y);
	void handleFocusLost();

	// Keeps the renderer's swapchain in step with the window. The framebuffer
	// size callback is the fast path, and the frame loop calls this too, so a
	// size change is never left unnoticed (pass 45).
	void syncRendererSize(std::uint32_t width, std::uint32_t height);

	void tick();
	void refreshDebugTitle();
	void setGamePaused(bool paused);
	void setActionState(Action action, bool down);
	void resetActionStates();

	GameWindow m_window;
	// The renderer's type is only complete in App.cpp; the deleter is defined
	// there so this header stays free of the Vulkan renderer include.
	struct RendererDeleter final {
		void operator()(vv::vulkan::VulkanRenderer* renderer) const;
	};
	std::unique_ptr<vv::vulkan::VulkanRenderer, RendererDeleter> m_renderer;
	vv::voxel::VoxelConfig m_voxelConfig;
	vv::core::Camera m_camera;
	vv::core::GameTimer m_gameTimer;
	std::vector<Binding> m_bindings;
	// The size the renderer's swapchain was last (re)created for.
	std::uint32_t m_rendererWidth = 0;
	std::uint32_t m_rendererHeight = 0;

	bool m_initialized = false;
	bool m_deviceLostReported = false;
	bool m_mouseLocked = false;
	bool m_ignoreNextMouseMove = false;
	double m_lastCursorX = 0.0;
	double m_lastCursorY = 0.0;
	bool m_haveLastCursor = false;

	// Action state, indexed by Action.
	bool m_actionDown[static_cast<std::size_t>(Action::Count)] = {};

	// Debug title refresh (1 Hz): shows the exact build id plus live render
	// state, so rendering reports from remote machines are unambiguous.
	double m_lastStatsSeconds = 0.0;
	int m_statFrames = 0;
	double m_titleFps = 0.0;
};

}  // namespace vv::core
