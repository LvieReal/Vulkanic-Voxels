#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "core/App.hpp"
#include "core/CommandLine.hpp"

// Thin entry point: the whole host lives in vv::core::App (GLFW window +
// Vulkan renderer + input + game loop). Failures are reported on stderr with
// a non-zero exit code - there is no toolkit dialog in the way any more.
//
// The command line is parsed here, once, before a window exists: --help must
// work without a display, and a typo must stop the run instead of starting it
// with different settings than the user asked for.
int main(int argc, char** argv) {
	const std::vector<std::string> arguments(argv + 1, argv + argc);
	const vv::core::CommandLineResult parsed =
			vv::core::parseCommandLine(arguments);
	if (!parsed.ok()) {
		std::fprintf(stderr, "[vv] fatal: %s\n", parsed.error.c_str());
		std::fprintf(stderr, "[vv] run with --help for the options\n");
		return 2;
	}
	if (parsed.options.help) {
		std::fputs(vv::core::commandLineUsage().c_str(), stdout);
		return 0;
	}
	vv::core::setOptions(parsed.options);

	// One line naming every switch that differs from the default: the thing a
	// bug report needs, and the confirmation that the flag was understood.
	const std::string active = vv::core::describeOptions(parsed.options);
	if (!active.empty()) {
		std::fprintf(stderr, "[vv] options: %s\n", active.c_str());
	}

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
