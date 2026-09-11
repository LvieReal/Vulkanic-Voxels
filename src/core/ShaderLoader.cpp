#include "core/ShaderLoader.h"

#include <fstream>

namespace vv::core {

std::vector<char> loadBinaryFile(const std::filesystem::path &path,
                                 std::string &outError) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    outError = "Failed to open file: " + path.string();
    return {};
  }

  const std::streamsize size = file.tellg();
  if (size <= 0) {
    outError = "File is empty: " + path.string();
    return {};
  }

  std::vector<char> buffer(static_cast<size_t>(size));
  file.seekg(0);
  file.read(buffer.data(), size);
  if (!file) {
    outError = "Failed to read file: " + path.string();
    return {};
  }

  return buffer;
}

} // namespace vv::core
