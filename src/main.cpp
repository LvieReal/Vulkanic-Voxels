#include <cstdio>
#include <exception>
#include <string>

#include "core/App.hpp"

// Thin entry point: the whole host lives in vv::core::App (GLFW window +
// Vulkan renderer + input + game loop). Failures are reported on stderr with
// a non-zero exit code - there is no toolkit dialog in the way any more.
int main(int argc, char** argv) {
	(void)argc;
	(void)argv;
	try {
		vv::core::App app;
		std::string error;
		if (!app.init(error)) {
			std::fprintf(stderr, "[vv] fatal: %s\n", error.c_str());
			return 1;
		}
		return app.run();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "[vv] fatal: %s\n", e.what());
		return 1;
	}
}
