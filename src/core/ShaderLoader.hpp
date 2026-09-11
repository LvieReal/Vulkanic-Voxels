#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vv::core {

std::vector<char> loadBinaryFile(const std::filesystem::path &path,
                                 std::string &outError);

} // namespace vv::core
