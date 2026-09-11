#include "core/RuntimePaths.h"

#include <windows.h>

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

} // namespace vv::core
