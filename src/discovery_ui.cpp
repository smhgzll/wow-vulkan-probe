#include "discovery_internal.hpp"
#include "overlay_uniform.hpp"
#include "overlay_menu.hpp"

namespace depth_probe {
uint32_t discovery_first_page(){return 4+(settings_controls().size()+9)/10;}
void populate_discovery_menu(VkDevice device,OverlayUniform& uniform){discovery::populate_explorer(device,uniform);}
bool discovery_menu_input(uint32_t page,uint32_t control,int direction,bool activate){return discovery::explorer_input(page,control,direction,activate);}
}
namespace depth_probe::discovery {
namespace { VkDevice menuDevice{}; }
void populate_explorer(VkDevice device,OverlayUniform& value) {
    if(value.panel[0]<discovery_first_page())return;
    const auto page=value.panel[0]-discovery_first_page();value.panel[3]=page+1;
    std::memset(value.rows,0,sizeof(value.rows));std::memset(value.explorer,0,sizeof(value.explorer));
    value.explorer[15]=discovery_enabled();
    auto row=[&](unsigned i,const char* label,float number,float kind=4) {
        value.rows[i][0]=number;value.rows[i][2]=float(setting_label(label));value.rows[i][3]=kind;
    };
    auto hash=[&](unsigned slot,uint64_t number){value.explorer[slot*2]=uint32_t(number);value.explorer[slot*2+1]=uint32_t(number>>32);};
    std::lock_guard lock(mutex);menuDevice=device;
    if(const auto open=devices.find(device);open!=devices.end())open->second.explorerUntil=open->second.frameSerial+120;   // the page lists the latest frame's draws
    const auto found=devices.find(device);const Device* s=found==devices.end()?nullptr:&found->second;
    const PipelineSummary* selected=nullptr;
    if(s)for(auto& p:s->latest)if(p.pipeline->serial==s->selected){selected=&p;break;}
    auto p=selected?selected->pipeline:nullptr;
    if(page==0) {
        row(0,"selected_pipeline",p?float(p->serial):0);
        row(1,"capture_remaining",s?s->requestedFrames:0);
        row(2,"pipeline_hash",0,p?8:5);row(3,"vertex_shader_hash",1,p&&p->vs?8:5);row(4,"fragment_shader_hash",2,p&&p->fs?8:5);
        row(5,"draws_last_interval",selected?selected->draws:0);
        row(6,"depth_write",selected && selected->depthWrite!=UINT32_MAX?selected->depthWrite:0,selected && selected->depthWrite!=UINT32_MAX?1:5);
        row(7,"alpha_blend",selected && selected->blend!=UINT32_MAX?selected->blend:0,selected && selected->blend!=UINT32_MAX?1:5);
        row(8,"render_target",selected?selected->targetKind:0,11);
        row(9,"isolation_mode",s?s->isolation:0,10);
        if(p){hash(0,p->hash);hash(1,p->vs?p->vs->hash:0);hash(2,p->fs?p->fs->hash:0);}
    } else {
        auto depth=selected?selected->target.depth:std::shared_ptr<View>{};
        row(0,"pipelines_seen",s?s->latest.size():0);
        row(1,"target_width",selected?selected->target.width:0);
        row(2,"target_height",selected?selected->target.height:0);
        row(3,"depth_format",depth?depth->format:0,depth?4:5);
        row(4,"depth_image",0,depth && depth->image?8:5);
        row(5,"bound_depth_images",selected?selected->depthBindings:0);
        row(6,"compare_samplers",selected?selected->compareSamplers:0);
        row(7,"fs_static_dref_ops",p && p->fs?p->fs->drefs:0,p && p->fs?4:5);
        row(8,"semantic_category",0,12);
        row(9,"metadata_incomplete",s && (s->limits || (p && !p->fingerprintComplete) || (selected && selected->incomplete))?1:0,1);
        if(depth && depth->image)hash(0,depth->image->handle);
    }
}
bool explorer_input(uint32_t page,uint32_t control,int direction,bool activate) {
    if(page<discovery_first_page())return false;
    if(!discovery_enabled())return true;
    std::lock_guard lock(mutex);auto found=devices.find(menuDevice);if(found==devices.end())return true;auto& s=found->second;
    if(activate && control==11) {
        if(!s.capture)s.requestedFrames=8;
        return true;
    }
    if(activate && control==12){s.isolation=0;s.isolationTargets.clear();return true;}
    if(page!=discovery_first_page())return true;
    auto selected=std::find_if(s.latest.begin(),s.latest.end(),[&](auto& p){return p.pipeline->serial==s.selected;});
    if(control==0 && (direction || activate) && !s.latest.empty()) {
        ptrdiff_t index=selected==s.latest.end()?0:selected-s.latest.begin();
        index=(index+(direction<0?-1:1)+s.latest.size())%s.latest.size();s.selected=s.latest[size_t(index)].pipeline->serial;
        s.isolation=0;s.isolationTargets.clear();return true;
    }
    if(control==9 && (direction || activate)) {
        const auto mode=(s.isolation+(direction<0?2:1))%3;
        s.isolation=0;s.isolationTargets.clear();
        if(!mode)return true;
        if(selected==s.latest.end() || !selected->pipeline->isolationSafe || !selected->target.known || selected->target.colors.size()!=1 || !selected->target.colors[0] || !selected->target.colors[0]->image) {
            log_line("[PIPELINE-ISOLATION-UNAVAILABLE] selected pipeline/target has unverified state or side effects");return true;
        }
        s.isolation=mode;s.isolationTargets.insert(selected->target.colors[0]->image->generation);s.isolationFrames=300;
        s.isolationDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        log_line("[PIPELINE-ISOLATION] pipeline="+std::to_string(s.selected)+" hash="+hex(selected->pipeline->hash)+" mode="+(mode==1?"hide-selected":"only-selected")+" scope=selected-color-image primary-one-time-direct-draws auto-stop=10s-or-300-presents");
    }
    return true;
}
}
