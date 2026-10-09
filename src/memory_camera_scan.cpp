#include "memory_camera_scan.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace depth_probe {
namespace {
constexpr uint64_t chunkBytes = 1ull << 20;
constexpr uint64_t maxRegionBytes = 512ull << 20;
constexpr uint64_t budgetBytes = 1536ull << 20;
constexpr size_t maxHits = 64, maxChainTargets = 8, maxHolders = 32, maxChains = 16;
// Static pointers of a non-relocated 32-bit exe live in its image; 3.3.5a's
// known globals sit well below this.
constexpr uint64_t staticBegin = 0x00400000, staticEnd = 0x02000000;
constexpr uint32_t maxStructOffset = 0x10000;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}

MemoryCameraScanner::~MemoryCameraScanner() {
    shared->stop = true;
    if (worker.joinable()) worker.join();
}

bool MemoryCameraScanner::start(const WowCameraLayout& layout) {
    {
        std::lock_guard lock(shared->mutex);
        if (shared->status.running) return false;
        shared->status.running = true;
        shared->status.finished = false;
        shared->status.bytesScanned = shared->status.bytesPlanned = 0;
        shared->status.note = "starting";
    }
    if (worker.joinable()) worker.join();
    worker = std::thread(run, shared, layout);
    return true;
}

MemoryCameraScanStatus MemoryCameraScanner::status() const {
    std::lock_guard lock(shared->mutex);
    return shared->status;
}

void MemoryCameraScanner::run(std::shared_ptr<Shared> sh, WowCameraLayout layout) {
    const auto& reader = self_memory_reader();
    const uint64_t span = layout.span();
    std::vector<uint8_t> buffer(chunkBytes + span);
    const uint64_t bufferBegin = uint64_t(uintptr_t(buffer.data())), bufferEnd = bufferBegin + buffer.size();

    std::vector<MemoryRegion> data, statics;
    uint64_t planned = 0;
    for (auto& r : read_self_maps()) {
        if (!r.readable) continue;
        if (r.begin < bufferEnd && bufferBegin < r.end) continue; // our own copy of whatever was read last
        if (r.begin >= staticBegin && r.end <= staticEnd) statics.push_back(r);
        const auto path = lower(r.path);
        if (!r.writable || !r.privateMapping) continue;
        if (!(path.empty() || path == "[heap]" || path.find(".exe") != std::string::npos)) continue;
        if (r.end - r.begin > maxRegionBytes || planned + (r.end - r.begin) > budgetBytes) continue;
        planned += r.end - r.begin;
        data.push_back(r);
    }
    {
        std::lock_guard lock(sh->mutex);
        sh->status.bytesPlanned = planned;
        sh->status.regions = data.size();
        sh->status.note = "scanning";
    }

    // Visits every region in fixed chunks; `overlap` extra bytes let a struct straddle a chunk boundary.
    auto visit = [&](const std::vector<MemoryRegion>& list, uint64_t overlap, bool progress, auto&& fn) {
        for (auto& r : list)
            for (uint64_t at = r.begin; at < r.end; at += chunkBytes) {
                if (sh->stop) return;
                const uint64_t size = std::min(chunkBytes + overlap, r.end - at);
                if (reader.read(at, buffer.data(), size_t(size))) fn(buffer.data(), size_t(size), at);
                if (progress) { std::lock_guard lock(sh->mutex); sh->status.bytesScanned += std::min(chunkBytes, r.end - at); }
            }
    };

    std::vector<CameraScanHit> hits;
    visit(data, span - 4, true, [&](const uint8_t* bytes, size_t size, uint64_t at) {
        if (hits.size() < maxHits) scan_camera_structs(bytes, size, at, layout, hits, maxHits);
    });

    // Who points at the first few hits, and which static exe address leads to those holders.
    std::vector<uint64_t> targets;
    for (auto& h : hits) if (targets.size() < maxChainTargets && h.address <= UINT32_MAX) targets.push_back(h.address);
    std::vector<std::pair<uint64_t, uint64_t>> holders; // (holder address, camera)
    if (!targets.empty())
        visit(data, 0, false, [&](const uint8_t* bytes, size_t size, uint64_t at) {
            for (size_t o = 0; o + 4 <= size && holders.size() < maxHolders; o += 4) {
                uint32_t v;
                std::memcpy(&v, bytes + o, 4);
                if (std::find(targets.begin(), targets.end(), uint64_t(v)) != targets.end()) holders.push_back({at + o, v});
            }
        });
    std::vector<MemoryCameraChain> chains;
    for (auto& [holder, camera] : holders) {
        if (chains.size() >= maxChains || holder > UINT32_MAX) break;
        visit(statics, 0, false, [&](const uint8_t* bytes, size_t size, uint64_t at) {
            std::vector<uint64_t> addresses;
            std::vector<uint32_t> offsets;
            find_pointers_to(bytes, size, at, uint32_t(holder), maxStructOffset, addresses, offsets, maxChains);
            for (size_t i = 0; i < addresses.size() && chains.size() < maxChains; ++i)
                chains.push_back({addresses[i], offsets[i], holder, camera});
        });
    }

    std::lock_guard lock(sh->mutex);
    sh->status.hits = std::move(hits);
    sh->status.chains = std::move(chains);
    sh->status.running = false;
    sh->status.finished = true;
    ++sh->status.generation;
    sh->status.note = sh->stop ? "stopped" : "done";
}
}
