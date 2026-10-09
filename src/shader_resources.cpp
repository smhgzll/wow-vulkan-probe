#include "shader_resources.hpp"
#include <cstdlib>
#include <fstream>

namespace depth_probe {

std::vector<uint32_t> read_spirv_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);

    if (!f)
        return {};

    const std::streamsize size = f.tellg();

    if (size < 20 || size > 16 * 1024 * 1024 || (size % 4) != 0)
        return {};

    f.seekg(0, std::ios::beg);

    std::vector<uint32_t> words(
        static_cast<size_t>(size) / sizeof(uint32_t));

    if (!f.read(reinterpret_cast<char*>(words.data()), size))
        return {};

    if (words[0] != 0x07230203 || words[1] > 0x00010600 || words[1] < 0x00010000 ||
        words[3] == 0 || words[4] != 0) return {};
    for (size_t i = 5; i < words.size();) {
        const size_t count = words[i] >> 16;
        if (!count || count > words.size() - i) return {};
        i += count;
    }
    return words;
}

std::string depth_shader_dir() {
    if (const char* p = std::getenv("DEPTH_PROBE_SHADER_DIR");
        p && *p) {
        return p;
    }

    return "/mnt/Games/Projects/vulkan-depth-probe/shaders";
}

} // namespace depth_probe
