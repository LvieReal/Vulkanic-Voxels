#pragma once

#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/GameWindow.hpp"

namespace vv::core {

// What a binding does, independent of which key carries it. Kept small and
// arithmetic-free so the binding logic is unit testable without a window.
enum class Action : std::uint8_t {
	MoveForward,
	MoveBack,
	MoveLeft,
	MoveRight,
	MoveUp,
	MoveDown,
	SpeedBoost,
	Count,
};

// One binding entry: the GLFW key a layout would put the label on (the
// layout-mapped channel) plus the PHYSICAL scancode of the position the
// binding should also answer to.
//
// Both channels are accepted (lookupAction), because which one a keyboard
// reports depends on the layout:
//   * the scancode identifies the POSITION, so the binding answers to the
//     same physical key on every layout - that is what makes WASD work on
//     AZERTY (ZQSD), QWERTZ and Dvorak keyboards;
//   * the GLFW key identifies the LABEL on the active layout, so a board
//     whose layout moves the key still moves the same way when the label
//     happens to be the bound one.
// A key that matches neither is a different key (the Z on QWERTZ reports
// key GLFW_KEY_Z and its own scancode, so it does not also walk forward).
//
// Scancodes come from defaultScancodes() below (evdev KEY_* on X11/Wayland,
// the Set-1 make code on Windows, the kVK_* code on macOS) and are compared
// against the values the windowing layer reports for the event.
// Platforms without a scancode (the value is -1) fall back to the label.
struct Binding final {
	Action action = Action::MoveForward;
	int key = GLFW_KEY_UNKNOWN;
	int scancode = -1;
};

// The default first-person bindings (the pass-42 control scheme):
//   WASD move, Space up, Ctrl down, Shift boost.
std::vector<Binding> defaultBindings();

// Looks a key event up in the bindings: the physical scancode first (so the
// position wins when a layout relocated the label), then the layout key.
// Returns Action::Count when the event is not bound to anything.
Action lookupAction(const std::vector<Binding>& bindings, int key, int scancode);

// Physical scancodes for the default bindings, per platform. The values are
// the ones GLFW reports in the key callback's `scancode` argument; -1 means
// "no scancode known on this platform", in which case the label key is the
// only channel.
//
//   X11 / Wayland: Linux evdev codes
//     (KEY_W 17, KEY_A 30, KEY_S 31, KEY_D 32, KEY_SPACE 57,
//      KEY_LEFTCTRL 29, KEY_LEFTSHIFT 42)
//   Windows:       Set-1 / PS-2 make codes
//     (W 0x11, A 0x1E, S 0x1F, D 0x20, Space 0x39, LControl 0x1D,
//      LShift 0x2A)
//   macOS:         Carbon virtual key codes
//     (W 13, A 0, S 1, D 2, Space 49, LControl 59, LShift 56)
//
// These are the same numbers GLFW puts into `scancode` on each platform, so
// the comparison is exact and needs no query per key.
struct Scancodes final {
	std::array<int, static_cast<std::size_t>(Action::Count)> values;

	int of(Action action) const {
		return values[static_cast<std::size_t>(action)];
	}
};

Scancodes defaultScancodes();

// The label channel for the default bindings (GLFW_KEY_*). These are the
// letters' layout keys, accepted as a secondary channel so a keyboard whose
// layout maps the physical position elsewhere still moves.
Scancodes defaultLabelKeys();

// Human-readable name of an action (for the startup log that records what the
// windowing/keyboard backend was detected as).
const char* actionName(Action action);

// Logs the detected backend, the label keys actually reported for the
// bindings and the platform scancodes, so "WASD does not work here" reports
// carry the numbers. `keyNameOf` maps a GLFW key to its layout name (the
// windowing layer passes glfwGetKeyName through).
void logBindings(const std::vector<Binding>& bindings,
								 const char* (*keyNameOf)(int key, int scancode));

}  // namespace vv::core
