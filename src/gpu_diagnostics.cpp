#include "overlay_internal.hpp"
#include "overlay_menu.hpp"
#include "depth_tracking.hpp"
#include "sync_tracking.hpp"
#include <cmath>

namespace depth_probe {
void create_gpu_timing(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st) {
    const auto flag=std::getenv("DEPTH_PROBE_GPU_TIMING");
    if(!flag || std::strcmp(flag,"1"))return;
    const auto unavailable=[] {log_line("[GPU-TIMING-UNAVAILABLE] query support/period/queue/allocation");};
    if(!data.get_properties || !data.get_queue_properties || !f.createQueryPool || !f.destroyQueryPool ||
       !f.cmdResetQueryPool || !f.cmdWriteTimestamp || !f.getQueryPoolResults) {unavailable();return;}
    VkPhysicalDeviceProperties properties{};data.get_properties(data.physical_device,&properties);
    uint32_t count=0;data.get_queue_properties(data.physical_device,&count,nullptr);
    if(!count || count>1024) {unavailable();return;}
    std::vector<VkQueueFamilyProperties> families(count);data.get_queue_properties(data.physical_device,&count,families.data());
    st.timestampBits=families[0].timestampValidBits;st.timestampPeriod=properties.limits.timestampPeriod;
    if(!st.timestampBits || st.timestampBits>64 || !std::isfinite(st.timestampPeriod) || st.timestampPeriod<=0 ||
       st.colorImages.empty() || st.colorImages.size()>1024) {unavailable();return;}
    VkQueryPoolCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=uint32_t(st.colorImages.size())*2;
    if(f.createQueryPool(data.device,&info,nullptr,&st.timestampPool)!=VK_SUCCESS) {st.timestampPool={};unavailable();return;}
    st.queryPending.resize(st.colorImages.size());
    log_line("[GPU-TIMING-READY] asynchronous per-image; periodNs="+std::to_string(st.timestampPeriod)+
             " validBits="+std::to_string(st.timestampBits)+" AO/SSR=unavailable-fused-pass");
}
VkResult collect_gpu_timing(const DeviceData& data, const DepthOverlayFns& f, DepthOverlayState& st, uint32_t image) {
    if(!st.timestampPool || !st.queryPending[image] || st.timingReadFailed || !overlay_settings().gpuTimingEnabled)return VK_SUCCESS;
    struct QueryResult { uint64_t ticks, available; } values[2]{};
    const auto result=f.getQueryPoolResults(data.device,st.timestampPool,image*2,2,sizeof(values),values,sizeof(QueryResult),
                                          VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(result==VK_ERROR_DEVICE_LOST)return result;
    if(result==VK_NOT_READY)return VK_SUCCESS;
    st.queryPending[image]=false;
    if(result!=VK_SUCCESS) {
        st.timingReadFailed=true;log_line("[GPU-TIMING-UNAVAILABLE] read result="+std::to_string(result));return VK_SUCCESS;
    }
    if(values[0].available && values[1].available) {
        const uint64_t mask=st.timestampBits==64?UINT64_MAX:(uint64_t(1)<<st.timestampBits)-1;
        const auto ticks=(values[1].ticks-values[0].ticks)&mask;
        st.overlayGpuMs=float(double(ticks)*st.timestampPeriod/1e6);
    }
    return VK_SUCCESS;
}
void populate_diagnostics(const DeviceData& data, const DepthOverlayState& st, OverlayUniform& uniform) {
    const uint32_t first=1+(settings_controls().size()+9)/10;
    if(uniform.panel[0]<first || uniform.panel[0]>=first+3)return;
    const auto page=uniform.panel[0]-first;
    const auto row=[&](unsigned index,const char* name,float value,bool available=true) {
        uniform.rows[index][0]=value;uniform.rows[index][2]=float(setting_label(name));uniform.rows[index][3]=available?4.f:5.f;
    };
    if(page==0) {
        unsigned candidates=0;
        {std::lock_guard lock(g_state_mutex);for(const auto& [key,c]:g_depth_candidates){(void)c;if(key.device==data.device)++candidates;}}
        row(0,"resolution_width",st.width);row(1,"resolution_height",st.height);
        row(2,"swapchain_format",st.format);row(3,"swapchain_images",st.colorImages.size());
        row(4,"depth_format",st.depthFormat);row(5,"depth_width",st.width);row(6,"depth_height",st.height);
        row(7,"depth_generation",float(st.depthGeneration));row(8,"depth_candidates",candidates);row(9,"frame_counter",float(st.frameNumber));
    } else if(page==1) {
        row(0,"post_fx_gpu_ms",st.overlayGpuMs,st.timestampPool && st.overlayGpuMs>=0 && !st.timingReadFailed && uniform.settings.gpuTimingEnabled);
        row(1,"ao_gpu_ms",0,false);row(2,"ssr_gpu_ms",0,false);
        row(3,"shader_reload_status",st.shaderReloadStatus,st.shaderReloadEnabled);
        row(4,"original_wait_count",st.originalWaitCount);
        row(5,"queue_wait_idle_count",data.sync?float(data.sync->overlayQueueWaitIdleCount):0);
        row(6,"scene_color_ready",!st.sceneColor.empty());row(7,"manual_projection_ready",uniform.panel[2]);
        row(8,"water_mask",0,false);row(9,"water_only_ssr",0,false);
    } else {
        row(0,"raw_center_sample",0);row(1,"raw_top_sample",1);row(2,"raw_left_sample",2);
        for(unsigned i=0;i<3;++i)uniform.rows[i][3]=6;
        row(3,"timestamp_valid_bits",st.timestampBits,bool(st.timestampPool));
        row(4,"timestamp_period_ns",st.timestampPeriod,bool(st.timestampPool));
        row(5,"depth_convention",uniform.settings.depthConvention);
        row(6,"projection_mode",uniform.settings.projectionMode);
        row(7,"near_plane",uniform.settings.nearPlane);row(8,"far_plane",uniform.settings.farPlane);row(9,"vertical_fov",uniform.settings.verticalFov);
    }
}
} // namespace depth_probe
