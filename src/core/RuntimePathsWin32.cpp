// Windows executable path resolution.
// The whole file is compiled out on non-Windows platforms (see
// RuntimePathsPosix.cpp).
#if defined(_WIN32)

#include "core/RuntimePaths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace vv::core {

std::filesystem::path executableDir() {
	std::wstring buffer;
	buffer.resize(4096);
	const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
																			 static_cast<DWORD>(buffer.size()));
	if (len == 0 || len >= buffer.size()) {
		return std::filesystem::current_path();
	}
	buffer.resize(len);
	return std::filesystem::path(buffer).parent_path();
}

}  // namespace vv::core

#endif  // defined(_WIN32)
