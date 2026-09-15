#pragma once

#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>

namespace vv::core {

// Owns the OS window and nothing else: creation, hints, size in physical
// pixels, the mouse-lock model and the raw events. The renderer, the input
// mapping and the frame loop live in App; this class exists so "the window"
// is one place when platform quirks (Wayland decoration fallbacks, focus-out,
// DPI changes) have to be handled.
//
// Mouse-lock model (carried over from the widget era): the cursor is
// disabled while playing (GLFW_CURSOR_DISABLED: hidden + reported at the
// window centre by GLFW), so callbacks deliver relative deltas; Escape
// releases it (GLFW_CURSOR_NORMAL) and pauses the game. lockMouse() also
// refocuses and re-centres the cursor so no first frame sees a jump.
//
// Window state: created hidden at half the monitor's work area, centered,
// maximized and then shown (pass 44) - the maximize request reaches the window
// manager before the window is mapped, so it comes up maximized with no flash
// and un-maximizing gives the half-size window back.
class GameWindow final {
 public:
	struct Hooks {
		void* user = nullptr;
		// Physical-pixel size (after the content-scale change), called
		// whenever the framebuffer size changes.
		void (*onFramebufferSize)(void* user, std::uint32_t width,
															std::uint32_t height) = nullptr;
		// A key/button/scroll event; `action` is GLFW_PRESS/RELEASE/REPEAT.
		void (*onKey)(void* user, int key, int scancode, int action,
									int mods) = nullptr;
		// Pointer motion; delta values are in screen pixels.
		void (*onCursorPos)(void* user, double x, double y) = nullptr;
		// A mouse button going down.
		void (*onMouseButton)(void* user, int button, int action,
													int mods) = nullptr;
		void (*onScroll)(void* user, double x, double y) = nullptr;
		// The window lost the keyboard/pointer focus (alt-tab, OS shortcut).
		void (*onFocusLost)(void* user) = nullptr;
	};

	GameWindow() = default;
	~GameWindow();

	GameWindow(const GameWindow&) = delete;
	GameWindow& operator=(const GameWindow&) = delete;

	// Brings up GLFW (if needed), creates the window with a Vulkan-ready
	// client API (GLFW_NO_API), applies the platform hints, installs the
	// hooks and enters the main loop on run().
	//
	// The window starts MAXIMIZED (decorated, so the window manager decides
	// what maximized means: work area minus panels) and un-maximizes to half
	// the monitor. The content scale and both sizes are logged.
	bool init(const Hooks& hooks, std::string& outError);

	// True once the user (or the compositor) asked the window and the loop
	// to close.
	bool shouldClose() const;

	// Requests the close flag (Escape-pause does not close by itself).
	void requestClose();

	// Blocks until at least one event has been processed. Call it once per
	// frame: with v-sync off the frame loop would otherwise spin on
	// glfwPollEvents and starve the compositor's own events.
	void waitEvents();

	// Physical pixels of the drawable area (framebuffer size), never 0.
	std::uint32_t framebufferWidth() const { return m_framebufferWidth; }
	std::uint32_t framebufferHeight() const { return m_framebufferHeight; }

	// Pumps events until the framebuffer size has stopped changing (three
	// identical samples, at least 30 ms apart in total) or maxWaitSeconds
	// elapsed, and keeps the reported size current.
	//
	// Called once, between showing the window and creating the swapchain: the
	// window manager answers the maximize request asynchronously (a
	// ConfigureNotify on X11, a wl_surface configure on Wayland), and creating
	// the swapchain before that answer arrives means creating it for the
	// RESTORE size while the window is really maximized - a stretched frame
	// until something else forces a resize.
	void settleFramebufferSize(double maxWaitSeconds = 0.25);

	// True when presenting makes sense: the window is on screen and not
	// iconified. Presenting to an invisible surface just burns swapchain
	// cycles (and an acquire that never completes), so the loop skips the
	// frame otherwise.
	bool canPresent() const;

	// Mouse lock: disables the cursor (relative motion) or restores it.
	// The lock is dropped automatically on focus loss.
	void setMouseLocked(bool locked);
	bool mouseLocked() const { return m_mouseLocked; }

	GLFWwindow* handle() const { return m_window; }

	// Sets the OS window title (used for the 1 Hz debug stats line).
	void setTitle(const std::string& title);

	// Swaps the renderer's back buffer onto the screen is the renderer's
	// business; this only tells whether that makes sense right now.
	bool minimized() const;

 private:
	void applyMainLoopHooks();

	static void framebufferSizeCallback(GLFWwindow* window, int width,
																		 int height);
	static void keyCallback(GLFWwindow* window, int key, int scancode,
													int action, int mods);
	static void cursorPosCallback(GLFWwindow* window, double x, double y);
	static void mouseButtonCallback(GLFWwindow* window, int button, int action,
																	int mods);
	static void scrollCallback(GLFWwindow* window, double x, double y);
	static void focusCallback(GLFWwindow* window, int focused);

	// The centre of the window's content area in screen coordinates.
	void centerCursorOnWindow();

	Hooks m_hooks{};
	GLFWwindow* m_window = nullptr;
	std::uint32_t m_framebufferWidth = 1;
	std::uint32_t m_framebufferHeight = 1;
	bool m_mouseLocked = false;
};

}  // namespace vv::core
