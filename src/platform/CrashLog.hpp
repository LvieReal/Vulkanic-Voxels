#pragma once

#include <string>

namespace vv::platform {

// Durable diagnostics for GUI-subsystem builds (the Windows Release
// target links -mwindows: stderr is detached from any console, so
// fprintf(stderr) breadcrumbs are invisible there - the pass-26.2
// "no logs in release" report). Crumbs append to
// <executable dir>/vv-crash.log, flushed immediately; the file is
// truncated at every launch, so after a crash it holds the CURRENT
// run's trail plus the exception record.
void crashLogClear();
void crashLogCrumb(const std::string& line);

// Windows: installs an unhandled-exception filter that appends the
// exception code and faulting address to the same file (the process
// still dies normally afterwards). Other platforms: no-op.
void installCrashHandler();

}  // namespace vv::platform
