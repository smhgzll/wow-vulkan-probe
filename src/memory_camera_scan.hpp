#pragma once
#include "memory_camera.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// On-demand (Ctrl+F8) background search of this process's writable memory
// for CGCamera-shaped structs (memory_camera.hpp's scan_camera_structs), then
// for pointers to each hit and for static (exe-image) pointers to those, to
// propose a pointer chain. Never runs per frame; bounded by a byte budget;
// only this object's own state is touched from the worker thread.
namespace depth_probe {
struct MemoryCameraChain {
    uint64_t staticAddress = 0;   // in the exe image
    uint32_t offset = 0;          // [staticAddress] + offset holds the camera pointer
    uint64_t pointerAddress = 0;  // = [staticAddress] + offset
    uint64_t camera = 0;
};
struct MemoryCameraScanStatus {
    bool running = false, finished = false;
    uint64_t generation = 0;      // increments on every finished scan
    uint64_t bytesScanned = 0, bytesPlanned = 0;
    size_t regions = 0;
    std::vector<CameraScanHit> hits;
    std::vector<MemoryCameraChain> chains;
    std::string note;
};
class MemoryCameraScanner {
public:
    ~MemoryCameraScanner();
    bool start(const WowCameraLayout&); // false if a scan is already running
    MemoryCameraScanStatus status() const;
private:
    struct Shared {
        mutable std::mutex mutex;
        MemoryCameraScanStatus status;
        std::atomic<bool> stop{false};
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::thread worker;
    static void run(std::shared_ptr<Shared>, WowCameraLayout);
};
}
