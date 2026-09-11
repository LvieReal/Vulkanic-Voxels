// POSIX executable path resolution (Linux, macOS, other Unix-likes).
// The whole file is compiled out on Windows (see RuntimePathsWin32.cpp).
#if !defined(_WIN32)

#include "core/RuntimePaths.hpp"

#include <climits>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

// PATH_MAX is POSIX, not C: it comes along with <climits> on glibc/macOS but
// is not guaranteed everywhere.
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace vv::core {

std::filesystem::path executableDir() {
#if defined(__APPLE__)
	// _NSGetExecutablePath may return a relative path; let filesystem resolve it.
	std::vector<char> buffer(PATH_MAX + 1, '\0');
	uint32_t size = static_cast<uint32_t>(buffer.size());
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
		buffer.resize(size + 1, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
			return std::filesystem::current_path();
		}
	}
	return std::filesystem::path(std::string(buffer.data()))
			.lexically_normal()
			.parent_path();
#elif defined(__linux__)
	char buffer[PATH_MAX + 1] = {};
	const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (len <= 0) {
		return std::filesystem::current_path();
	}
	buffer[len] = '\0';
	return std::filesystem::path(buffer).parent_path();
#else
	// No known way to query the executable path on this platform; fall back to
	// the current working directory (resources are expected next to it).
	return std::filesystem::current_path();
#endif
}

}  // namespace vv::core

#endif  // !defined(_WIN32)
