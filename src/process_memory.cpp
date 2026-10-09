#include "process_memory.hpp"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/uio.h>
#include <unistd.h>

namespace depth_probe {
namespace {
class SelfMemoryReader final : public MemoryReader {
public:
    bool read(uint64_t address, void* out, size_t size) const override {
        if (!size) return true;
        if (!address || address > uint64_t(UINTPTR_MAX) - size) return false;
        if (mode.load(std::memory_order_relaxed) != useProcMem) {
            iovec local{out, size};
            iovec remote{reinterpret_cast<void*>(uintptr_t(address)), size};
            const ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
            if (n == ssize_t(size)) return true;
            // EFAULT/partial = address not (fully) mapped: a normal failed read.
            if (n >= 0 || (errno != ENOSYS && errno != EPERM)) return false;
            mode.store(useProcMem, std::memory_order_relaxed);
        }
        static const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        return pread64(fd, out, size, off64_t(address)) == ssize_t(size);
    }
private:
    static constexpr int useProcMem = 2;
    const pid_t pid = getpid();
    mutable std::atomic<int> mode{0};
};
}

const MemoryReader& self_memory_reader() {
    static const SelfMemoryReader reader;
    return reader;
}

std::vector<MemoryRegion> parse_proc_maps(const std::string& text) {
    std::vector<MemoryRegion> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string range, perms, offset, dev, inode;
        if (!(fields >> range >> perms >> offset >> dev >> inode) || perms.size() < 4) continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        MemoryRegion r;
        char* end = nullptr;
        r.begin = std::strtoull(range.c_str(), &end, 16);
        r.end = std::strtoull(range.c_str() + dash + 1, &end, 16);
        if (r.end <= r.begin) continue;
        r.readable = perms[0] == 'r';
        r.writable = perms[1] == 'w';
        r.privateMapping = perms[3] == 'p';
        std::getline(fields >> std::ws, r.path);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<MemoryRegion> read_self_maps() {
    std::ifstream maps("/proc/self/maps");
    std::stringstream text;
    text << maps.rdbuf();
    return parse_proc_maps(text.str());
}
}
