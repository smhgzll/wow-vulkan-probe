#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Read-only access to this process's own memory (the layer runs inside the
// same Linux process Wine uses for wow.exe, so game addresses are directly
// addressable). Reads never dereference a raw pointer: an unmapped or
// unreadable address is a failed read, never a fault. Nothing here writes.
namespace depth_probe {
struct MemoryReader {
    virtual ~MemoryReader() = default;
    // All-or-nothing: false (out contents unspecified) unless every byte was read.
    virtual bool read(uint64_t address, void* out, size_t size) const = 0;
    bool read_u32(uint64_t address, uint32_t& out) const { return read(address, &out, sizeof out); }
};

// process_vm_readv on our own pid, falling back to pread on /proc/self/mem.
const MemoryReader& self_memory_reader();

struct MemoryRegion {
    uint64_t begin = 0, end = 0;
    bool readable = false, writable = false, privateMapping = false;
    std::string path;
};
std::vector<MemoryRegion> parse_proc_maps(const std::string& text);
std::vector<MemoryRegion> read_self_maps();
}
