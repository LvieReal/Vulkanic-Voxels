#include "platform/CrashLog.hpp"

#include "core/RuntimePaths.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

std::filesystem::path crashLogPath() {
	const std::filesystem::path dir = vv::core::executableDir();
	return dir.empty() ? std::filesystem::path("vv-crash.log")
	                   : dir / "vv-crash.log";
}

std::string timestamp() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::ostringstream os;
	os << std::put_time(std::localtime(&t), "%H:%M:%S");
	return os.str();
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

LONG WINAPI vvCrashFilter(EXCEPTION_POINTERS* info) {
	try {
		std::ofstream f(crashLogPath(), std::ios::app);
		if (f && info && info->ExceptionRecord) {
			f << '[' << timestamp() << "] UNHANDLED EXCEPTION code=0x"
			  << std::hex << info->ExceptionRecord->ExceptionCode
			  << " addr=0x"
			  << reinterpret_cast<std::uintptr_t>(
			         info->ExceptionRecord->ExceptionAddress)
			  << std::dec << '\n';
		}
	} catch (...) {
		// Never throw inside the crash handler.
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

namespace vv::platform {

void crashLogClear() {
	std::ofstream f(crashLogPath(), std::ios::trunc);
}

void crashLogCrumb(const std::string& line) {
	std::ofstream f(crashLogPath(), std::ios::app);
	if (!f) {
		return;
	}
	f << '[' << timestamp() << "] " << line << '\n';
	// ofstream closes + flushes on scope exit.
}

void installCrashHandler() {
#ifdef _WIN32
	SetUnhandledExceptionFilter(&vvCrashFilter);
#endif
}

}  // namespace vv::platform
