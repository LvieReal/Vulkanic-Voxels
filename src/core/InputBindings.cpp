#include "core/InputBindings.hpp"

#include <cstdio>

namespace vv::core {

namespace {

// Physical positions, per platform. See the header for the sources of these
// numbers (evdev / PS-2 Set 1 / Carbon virtual key codes) and the note that
// they are exactly what GLFW reports as `scancode`.
#if defined(_WIN32)
constexpr int kScancodeForward = 0x11;  // W
constexpr int kScancodeLeft = 0x1E;     // A
constexpr int kScancodeBack = 0x1F;     // S
constexpr int kScancodeRight = 0x20;    // D
constexpr int kScancodeUp = 0x39;       // Space
constexpr int kScancodeDown = 0x1D;     // Left Ctrl
constexpr int kScancodeBoost = 0x2A;    // Left Shift
#elif defined(__APPLE__)
constexpr int kScancodeForward = 13;  // W
constexpr int kScancodeLeft = 0;      // A
constexpr int kScancodeBack = 1;      // S
constexpr int kScancodeRight = 2;     // D
constexpr int kScancodeUp = 49;       // Space
constexpr int kScancodeDown = 59;     // Left Ctrl
constexpr int kScancodeBoost = 56;    // Left Shift
#else
// X11 and Wayland both report evdev codes.
constexpr int kScancodeForward = 17;  // KEY_W
constexpr int kScancodeLeft = 30;     // KEY_A
constexpr int kScancodeBack = 31;     // KEY_S
constexpr int kScancodeRight = 32;    // KEY_D
constexpr int kScancodeUp = 57;       // KEY_SPACE
constexpr int kScancodeDown = 29;     // KEY_LEFTCTRL
constexpr int kScancodeBoost = 42;    // KEY_LEFTSHIFT
#endif

constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

}  // namespace

std::vector<Binding> defaultBindings() {
	std::vector<Binding> bindings;
	bindings.reserve(kActionCount);
	bindings.push_back({Action::MoveForward, GLFW_KEY_W, kScancodeForward});
	bindings.push_back({Action::MoveBack, GLFW_KEY_S, kScancodeBack});
	bindings.push_back({Action::MoveLeft, GLFW_KEY_A, kScancodeLeft});
	bindings.push_back({Action::MoveRight, GLFW_KEY_D, kScancodeRight});
	bindings.push_back({Action::MoveUp, GLFW_KEY_SPACE, kScancodeUp});
	bindings.push_back({Action::MoveDown, GLFW_KEY_LEFT_CONTROL, kScancodeDown});
	bindings.push_back({Action::SpeedBoost, GLFW_KEY_LEFT_SHIFT, kScancodeBoost});
	return bindings;
}

Action lookupAction(const std::vector<Binding>& bindings, int key,
										int scancode) {
	for (const Binding& binding : bindings) {
		// Physical position first (layout independence), then the label:
		// which of the two a keyboard produces depends on the layout, and a
		// key on the other channel is simply a different key.
		if (binding.scancode >= 0 && scancode == binding.scancode) {
			return binding.action;
		}
		if (key != GLFW_KEY_UNKNOWN && key == binding.key) {
			return binding.action;
		}
	}
	return Action::Count;
}

Scancodes defaultScancodes() {
	Scancodes codes{};
	codes.values.fill(-1);
	codes.values[static_cast<std::size_t>(Action::MoveForward)] =
			kScancodeForward;
	codes.values[static_cast<std::size_t>(Action::MoveBack)] = kScancodeBack;
	codes.values[static_cast<std::size_t>(Action::MoveLeft)] = kScancodeLeft;
	codes.values[static_cast<std::size_t>(Action::MoveRight)] = kScancodeRight;
	codes.values[static_cast<std::size_t>(Action::MoveUp)] = kScancodeUp;
	codes.values[static_cast<std::size_t>(Action::MoveDown)] = kScancodeDown;
	codes.values[static_cast<std::size_t>(Action::SpeedBoost)] =
			kScancodeBoost;
	return codes;
}

Scancodes defaultLabelKeys() {
	Scancodes keys{};
	keys.values.fill(GLFW_KEY_UNKNOWN);
	keys.values[static_cast<std::size_t>(Action::MoveForward)] = GLFW_KEY_W;
	keys.values[static_cast<std::size_t>(Action::MoveBack)] = GLFW_KEY_S;
	keys.values[static_cast<std::size_t>(Action::MoveLeft)] = GLFW_KEY_A;
	keys.values[static_cast<std::size_t>(Action::MoveRight)] = GLFW_KEY_D;
	keys.values[static_cast<std::size_t>(Action::MoveUp)] = GLFW_KEY_SPACE;
	keys.values[static_cast<std::size_t>(Action::MoveDown)] =
			GLFW_KEY_LEFT_CONTROL;
	keys.values[static_cast<std::size_t>(Action::SpeedBoost)] =
			GLFW_KEY_LEFT_SHIFT;
	return keys;
}

const char* actionName(Action action) {
	switch (action) {
		case Action::MoveForward:
			return "move forward";
		case Action::MoveBack:
			return "move back";
		case Action::MoveLeft:
			return "move left";
		case Action::MoveRight:
			return "move right";
		case Action::MoveUp:
			return "move up";
		case Action::MoveDown:
			return "move down";
		case Action::SpeedBoost:
			return "speed boost";
		case Action::Count:
			break;
	}
	return "?";
}

void logBindings(const std::vector<Binding>& bindings,
								 const char* (*keyNameOf)(int key, int scancode)) {
	for (const Binding& binding : bindings) {
		const char* label = keyNameOf != nullptr ? keyNameOf(binding.key, 0)
																						 : nullptr;
		std::fprintf(stderr, "[vv] key %-12s: key %d (%s), scancode %d\n",
								 actionName(binding.action), binding.key,
								 label != nullptr ? label : "no layout name",
								 binding.scancode);
	}
}

}  // namespace vv::core
