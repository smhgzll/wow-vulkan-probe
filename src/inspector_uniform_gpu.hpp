#pragma once
#include "surface_inspector.hpp"
#include <condition_variable>

namespace depth_probe::discovery {
struct GpuUniformBatch {
    DeviceData data;
    VkBuffer buffer{};VkDeviceMemory memory{};VkQueryPool marker{};
    void* mapped=nullptr;uint64_t used=0;
    bool coherent=false,recorded=false,completed=false;
    std::mutex submitMutex;std::condition_variable submitChanged;
    bool submitting=false,submitted=false;
    std::vector<std::weak_ptr<GpuUniformRead>> reads;
    ~GpuUniformBatch();
    void release();
    bool poll();
};
struct GpuUniformRead {
    std::shared_ptr<GpuUniformBatch> batch;
    std::shared_ptr<UniformSnapshot> snapshot;
    uint64_t destination=0;
};
void schedule_gpu_uniforms(const DeviceData&,Device&,Command&,Draw&,const std::vector<ResourceUse>&);
// Schedules a GPU-side readback of ONE specific resource into `c`'s shared
// uniform batch (created lazily on first use, reused across resources/draws
// within the same command buffer). The read completes asynchronously - poll
// the returned object's `batch->poll()` once it is known to be submitted
// (or use `poll_gpu_uniforms(Frame&)` for a whole frame's worth). This is
// the reusable core behind schedule_gpu_uniforms (Inspector's pick-triggered
// multi-resource scheduling); Scene Debugger's targeted water-anchor
// readback (scene_debugger.cpp) calls it directly for one known binding,
// independent of any Inspector pick. `budgetUsed`/`budgetMax` is the
// caller's own running byte budget, matching try_capture_uniform's pattern.
std::shared_ptr<GpuUniformRead> schedule_gpu_uniform_read(const DeviceData&,Device&,Command&,const Draw&,const ResourceUse&,size_t& budgetUsed,size_t budgetMax);
void record_gpu_uniforms(const DeviceData&,Device&,Command&,VkCommandBuffer);
bool poll_gpu_uniforms(Frame&);
void release_gpu_uniforms(Device&);
std::vector<std::shared_ptr<GpuUniformBatch>> begin_uniform_submit(const Frame&);
void end_uniform_submit(const std::vector<std::shared_ptr<GpuUniformBatch>>&,VkResult);
void retire_uniform_source(VkDevice,uint64_t buffer,uint64_t memoryGeneration=0);
}
