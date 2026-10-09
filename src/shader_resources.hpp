#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace depth_probe {
std::vector<uint32_t> read_spirv_file(const std::string& path);
std::string depth_shader_dir();
} // namespace depth_probe
