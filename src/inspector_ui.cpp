#include "volumetric_fog.hpp"
#include "water_reflections.hpp"
#include "surface_inspector.hpp"
#include "overlay_internal.hpp"
#include "matrix_analyzer.hpp"
#include "scene_debugger.hpp"
#include <cmath>
#include <cstdio>

namespace depth_probe {
using namespace discovery;
namespace {VkDevice activeDevice{};
void line(OverlayUniform& u,unsigned row,const std::string& text) {
    if(row>=32)return;
    // A page may replace its common header/status. Packed characters must be
    // overwritten, not OR-ed with the previous text (which corrupts the label).
    std::fill_n(u.inspectorText+row*12,12,0u);
    for(size_t i=0;i<std::min(size_t(48),text.size());++i) {
        auto c=uint8_t(std::toupper(static_cast<unsigned char>(text[i])));auto index=row*48+i;
        u.inspectorText[index/4]|=uint32_t(c)<<((index%4)*8);
    }
}
// Evidence strings are full sentences (meant to be read, per the Scene
// Debugger's "explain the confidence" requirement) but each panel row holds
// only 48 characters. Wraps on word boundaries across consecutive rows
// rather than silently truncating; stops at maxRow (inclusive) and drops
// anything past it rather than overflowing into unrelated UI below.
void wrap_lines(OverlayUniform& u,unsigned& row,unsigned maxRow,const std::string& text) {
    size_t pos=0;
    while(pos<text.size() && row<=maxRow) {
        size_t take=std::min(size_t(48),text.size()-pos);
        if(pos+take<text.size()) {
            size_t space=text.rfind(' ',pos+take);
            if(space!=std::string::npos && space>pos)take=space-pos;
        }
        line(u,row++,text.substr(pos,take));
        pos+=take;
        while(pos<text.size() && text[pos]==' ')++pos;
    }
}
std::string identity(uint64_t v){return v?hex(v):"UNKNOWN";}
std::string number(float v) {char b[32];std::snprintf(b,sizeof(b),"%.7f",double(v));return b;}
// Short "M4:<tag> <confidence>" suffix for a buffer line, when this exact
// binding has a captured snapshot with a mathematically interesting 4x4
// candidate in it. Single-snapshot only - see matrix_analyzer.hpp for what
// the tag can and cannot claim. Empty string if there is nothing to show.
std::string matrix_hint(const Inspector& in,uint32_t set,uint32_t binding,uint32_t element) {
    if(!in.selected)return "";
    for(const auto& snap:in.selected->uniforms) {
        if(snap->set!=set || snap->binding!=binding || snap->element!=element || snap->bytes.empty())continue;
        auto top=find_matrix_candidates(snap->bytes,1);
        if(top.empty())return "";
        const auto& c=top.front();
        char pct[8];std::snprintf(pct,sizeof(pct),"%.0f%%",double(c.confidence*100));
        return " M4:"+c.classification.substr(0,c.classification.find(' '))+" "+pct;
    }
    return "";
}
// CONFIRMED only after an explicit user Accept - heuristic ranking alone
// never claims certainty, matching the rest of this project's stance on
// unverified classification.
std::string finding_status(bool any,float confidence,int verdict) {
    if(!any)return "UNKNOWN";
    if(verdict==1)return "CONFIRMED";
    if(verdict==-1)return "REJECTED";
    if(confidence>=.6f)return "LIKELY";
    if(confidence>=.3f)return "POSSIBLE";
    return "POSSIBLE (WEAK)";
}
// Temporary diagnostic (2026-09): pinpoints exactly which stage of the
// water-anchor pipeline is blocking a result, since "no candidate appeared"
// alone doesn't say whether the shader was never seen, the binding wasn't
// found, capture failed (and which path), or the math rejected the bytes.
std::string water_last_error(const WaterCameraAnchor& a) {
    if(!a.vsSeen)return "water shader not observed yet - look at visible water";
    if(!a.bindingFound)return "S2/B0 not found on the water draw";
    const bool hostFailed=a.hostCaptureStatus.rfind("failed",0)==0;
    const bool gpuFailed=a.gpuReadbackStatus.rfind("failed",0)==0;
    if(hostFailed && gpuFailed)return "host: "+a.hostCaptureStatus+" | gpu: "+a.gpuReadbackStatus;
    if(hostFailed && a.gpuReadbackStatus=="pending")return "host: "+a.hostCaptureStatus+" (gpu readback pending)";
    if(a.cameraMathStatus.rfind("failed",0)==0)return a.cameraMathStatus;
    return a.valid?"none":"waiting for capture";
}
std::string level_label(CameraEvidenceLevel level) {
    switch(level) {
    case CameraEvidenceLevel::GpuVerifiedWater:return "GPU VERIFIED (WATER ANCHOR)";
    case CameraEvidenceLevel::GpuTemporal:return "GPU TEMPORAL EVIDENCE";
    case CameraEvidenceLevel::MemoryCandidate:return "MEMORY CANDIDATE";
    case CameraEvidenceLevel::MemoryGpuMatch:return "MEMORY <-> GPU MATCH";
    case CameraEvidenceLevel::Verified:return "VERIFIED";
    default:return "HEURISTIC (SINGLE SNAPSHOT)";
    }
}
std::vector<const ResourceUse*> textures(const Inspector& in) {
    std::vector<const ResourceUse*> out;if(in.selected)for(auto& r:in.selected->resources)if(r.descriptor.view && r.descriptor.view->image)out.push_back(&r);return out;
}
std::vector<uint32_t> actions(uint32_t page) {
    switch(page){case 0:return {5,8,11,14,17,22,24,26,28};case 12:{std::vector<uint32_t> rows;for(uint32_t i=0;i<shade_menu_rows();++i)rows.push_back(4+2*i);rows.push_back(30);return rows;}case 11:{std::vector<uint32_t> rows;for(uint32_t i=0;i<fog_menu_rows();++i)rows.push_back(4+i);rows.push_back(30);return rows;}case 2:return {21,22,23,25,27,29,30};case 3:return {4,16,28,29,30};case 4:return {25,30};case 5:return {8,11,14,30};case 7:return {5,8,11,14,20,21,23,25,26,27,30};case 8:{std::vector<uint32_t> rows;for(uint32_t i=0;i<waterLookOptionCount+1;++i)rows.push_back(5+i);rows.push_back(7+uint32_t(waterLookOptionCount));rows.push_back(30);return rows;}case 9:return {17,19,30};case 10:return {22,23,25,26,28,29,30};default:return {30};}
}
bool can_highlight(const Inspector& in) {
    if(!in.materialIdentity || !in.selected || !in.selected->draw.pipeline)return false;
    const auto& d=in.selected->draw;const auto e=effective_state(d);
    return (e.valid&Blend) && !e.blend && d.pipeline->inspector && d.pipeline->inspector->tint;
}
std::string inspector_format(VkFormat f) {
    switch(f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:return "BC1";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:return "BC1 SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK:case VK_FORMAT_BC2_SRGB_BLOCK:return "BC2";
    case VK_FORMAT_BC3_UNORM_BLOCK:case VK_FORMAT_BC3_SRGB_BLOCK:return "BC3";
    case VK_FORMAT_BC4_UNORM_BLOCK:return "BC4";case VK_FORMAT_BC5_UNORM_BLOCK:return "BC5";
    case VK_FORMAT_BC7_UNORM_BLOCK:case VK_FORMAT_BC7_SRGB_BLOCK:return "BC7";
    case VK_FORMAT_R8G8B8A8_UNORM:return "RGBA8";case VK_FORMAT_R8G8B8A8_SRGB:return "RGBA8 SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM:return "BGRA8";case VK_FORMAT_B8G8R8A8_SRGB:return "BGRA8 SRGB";
    case VK_FORMAT_D24_UNORM_S8_UINT:return "D24S8";case VK_FORMAT_D32_SFLOAT:return "D32F";
    default:return "VK "+std::to_string(f);
    }
}
std::string filter_name(uint32_t f){return f==VK_FILTER_NEAREST?"NEAREST":f==VK_FILTER_LINEAR?"LINEAR":"OTHER";}
std::string address_name(uint32_t a){return a==0?"REPEAT":a==1?"MIRROR":a==2?"EDGE":a==3?"BORDER":"OTHER";}
void page_action(Device& state,Inspector& in,unsigned row,bool& pick) {
    if(row==30){in.page=in.page==8?7:0;return;}
    if(in.page==0) {
        if(row==5)in.page=1;
        if(row==8)in.page=2;
        if(row==11)in.page=5;
        if(row==14)in.page=6;
        if(row==26)in.page=7;
        if(row==17) {
            in.geometryMode=!in.geometryMode;in.selected.reset();in.geometryCandidates.clear();
            in.geometryCandidate=in.finalVisible=in.highlight=in.preview=in.armed=in.staged=in.captureDepth=false;
            in.frame={};in.completedTicket=0;in.reason="Pick mode changed - click the world again";
        }
        if(row==22)in.page=9;
        if(row==28)in.page=11;
        if(row==24)in.page=12;
    } else if(in.page==11) {
        if(row>=4 && row-4<fog_menu_rows())fog_menu_click(state,row-4);
    } else if(in.page==12) {
        if(row>=4 && (row-4)%2==0 && (row-4)/2<shade_menu_rows())shade_menu_click(state,(row-4)/2);
    } else if(in.page==9) {
        if(row==17)in.page=10;
        if(row==19)in.page=0;
    } else if(in.page==10) {
        auto& sd=scene_debugger_state(state);
        const auto n=uint32_t(sd.cameras.size());
        if(row==22 && n)sd.cameraIndex=(sd.cameraIndex+1)%n;
        if(row==23 && n)sd.cameraIndex=(sd.cameraIndex+n-1)%n;
        if(row==25 && sd.cameraIndex<n)sd.cameras[sd.cameraIndex].verdict=1;
        if(row==26 && sd.cameraIndex<n)sd.cameras[sd.cameraIndex].verdict=-1;
        if(row==28)in.page=9;
        if(row==29)in.page=0;
    } else if(in.page==2) {
        if((row==21 || row==22) && !in.geometryCandidates.empty()) {
            const auto n=uint32_t(in.geometryCandidates.size());
            select_geometry_candidate(in,(in.candidateIndex+(row==21?1:n-1))%n);
        }
        if(row==23)in.highlight=can_highlight(in) && !in.highlight;
        if(row==25)in.page=3;
        if(row==27)in.page=4;
        if(row==29)in.page=1;
    } else if(in.page==3) {
        auto values=textures(in);
        if(row==4 && !values.empty()){in.texture=(in.texture+1)%values.size();in.preview=false;}
        if(row==16)in.preview=!in.preview;
        if(row==28)in.page=5;
        if(row==29)in.page=2;
    } else if(in.page==4 && row==25)request_asset_export(in,false,true);
    else if(in.page==5) {
        if(row==8)request_asset_export(in,false);
        if(row==11)request_asset_export(in,true);
        if(row==14)request_asset_export(in,false,true);
    } else if(in.page==7) {
        auto& water=water_state(state);
        if(row==5)select_water_surface(state,in);
        if(row==8 && water.selectedVS){water.mode=(water.mode+1)%3;water.status=water.mode?"Waiting for a supported water pass":"Reflections off";water_settings_save(water);}
        if(row==11)water.debug=(water.debug+1)%waterDebugViewCount;
        if(row==14){water.strength+=.15f;if(water.strength>1.01f)water.strength=.15f;water_settings_save(water);}
        if(row==20){water.captureArmed=true;water.status="Capture armed - next water pass is saved";}
        if(row==21){water.style=water.style?0:1;water_settings_save(water);}   // A/B: MODERN <-> CLASSIC
        if(row==23){static const uint32_t presets[]{waterLayersAll,waterLayerReflection,waterLayerReflection|waterLayerRefraction,waterLayerReflection|waterLayerDepthTint|waterLayerFoam,waterLayerRefraction|waterLayerDepthTint|waterLayerFoam};
            size_t next=0;for(size_t i=0;i<std::size(presets);++i)if(presets[i]==water.layers)next=(i+1)%std::size(presets);water.layers=presets[next];water_settings_save(water);}
        if(row==25){water.tone=(water.tone+1)%waterToneCount;water_settings_save(water);}
        if(row==26)in.page=8;
        if(row==27){g_perf_ab_request.store(true);water.status="Performance test armed: close the menu (F10) and stand still for 40 s";}
    } else if(in.page==8) {
        auto& water=water_state(state);
        if(row>=5 && row-5<waterLookOptionCount){water_look_cycle(water.look,row-5);water_settings_save(water);}
        if(row==5+waterLookOptionCount){water_look_cycle_quality(water.look);water_settings_save(water);}
        if(row==7+waterLookOptionCount){water.look=WaterLook{};water_settings_save(water);}
    } else if(in.page==1)pick=true;
}
}
void inspector_context(VkDevice device,DepthOverlayState& state) {
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);activeDevice=device;auto& s=device_state(device);auto& in=inspector_state(s,device);
    if(in.swapchain!=state.swapchain || in.depthGeneration!=state.depthGeneration || in.width!=state.width || in.height!=state.height) {
        in.armed=in.staged=in.captureDepth=in.depthReady=in.highlight=in.preview=false;in.completedTicket=0;
        in.selected.reset();in.geometryCandidates.clear();in.geometryCandidate=in.finalVisible=false;in.frame={};in.reason="Scene changed - inspect again";
        if(s.water && !s.water->profileSaved){s.water->mode=0;s.water->selectedVS=s.water->selectedFS=s.water->selectedSignature=0;s.water->status="Scene changed - select water again";}
    }
    in.swapchain=state.swapchain;in.width=state.width;in.height=state.height;in.depthImage=id(state.depthImage);in.depthGeneration=state.depthGeneration;
    inspector_poll(s,in);
}
bool inspector_menu_input(OverlayMenu& menu,const MenuInput& input,bool click) {
    if(!inspector_enabled() || !activeDevice)return false;
    uint32_t x=0,y=0;bool pick=false;
    {
        std::lock_guard lock(mutex);auto& in=inspector_state(device_state(activeDevice),activeDevice);
        if(!menu.visible){in.page=0;in.armed=in.staged=in.captureDepth=in.highlight=in.preview=false;return true;}
        if(in.page==6) {
            // Advanced keeps the existing menu untouched. F10/ESC returns home.
            return false;
        }
        const bool inPanel=input.x>=0 && input.x<menu_width && input.y>=0 && input.y<(in.page==1?124:menu_height);
        // Home and Selected Surface also accept world clicks. Previously those
        // clicks were silently discarded unless Inspect Surface was re-armed.
        const bool worldPage=in.page==0 || in.page==1 || in.page==2;
        if(click && input.pointerValid && worldPage && !inPanel) {
                const float scale=menu_scale(in.width,in.height);
                const float px=(input.x+menu_margin)*scale,py=(input.y+menu_margin)*scale;
                if(std::isfinite(px)&&std::isfinite(py)&&px>=0&&py>=0&&px<in.width&&py<in.height){x=uint32_t(std::floor(px));y=uint32_t(std::floor(py));pick=true;}
        } else if(in.page!=1) {
            auto controls=actions(in.page);auto found=std::find(controls.begin(),controls.end(),menu.selected);
            size_t current=found==controls.end()?0:size_t(found-controls.begin());
            if(input.key==MenuKey::Up)current=(current+controls.size()-1)%controls.size();
            if(input.key==MenuKey::Down)current=(current+1)%controls.size();
            menu.selected=controls[current];
            bool activate=input.key==MenuKey::Activate;
            if(click && input.pointerValid && input.x>=16 && input.x<390 && input.y>=30 && input.y<610) {
                menu.selected=uint32_t((input.y-30)/18);activate=true;
            }
            if(activate){
                const auto previous=in.page;
                page_action(device_state(activeDevice),in,menu.selected,pick);
                log_line("[INSPECTOR-ACTION] page="+std::to_string(previous)+" row="+std::to_string(menu.selected)+" next="+std::to_string(in.page));
                if(in.page==6){menu.page=discovery_first_page();menu.selected=0;}
            }
        }
        menu.dragging=-1;
    }
    if(pick)inspector_request_pick(activeDevice,x,y);
    return true;
}
void populate_inspector_menu(VkDevice device,OverlayUniform& u) {
    if(!inspector_enabled())return;
    std::lock_guard lock(mutex);auto& s=device_state(device);auto& in=inspector_state(s,device);
    if(in.page==6)return;
    std::memset(u.inspectorText,0,sizeof(u.inspectorText));
    const bool marker=in.armed || in.staged || (in.ticket && in.completedTicket==in.ticket);
    u.inspector[0]=1;u.inspector[1]=in.page;u.inspector[2]=marker?(in.selected?2u:1u):0u;u.inspector[3]=in.highlight;
    u.pick[0]=in.x;u.pick[1]=in.y;
    if(in.captureDepth && !in.depthReady)u.pick[2]=in.ticket;
    line(u,0,"GRAPHICS INSPECTOR");
    line(u,2,in.geometryMode?"SCOPE: DEPTH-TESTED GEOMETRY CANDIDATES":"SCOPE: LAST SCENE PASS / PROTOTYPE");
    line(u,30,"[ BACK TO HOME ]");line(u,32-1,u.menu[3]==2?"INPUT SHARED WITH GAME - ESC / F10 CLOSE":u.menu[3]==0?"NO INPUT BACKEND - INSPECTION UNAVAILABLE":"ESC / F10 CLOSES AND RELEASES GAME INPUT");
    if(in.page==0) {
        line(u,5,"[ INSPECT SURFACE ]");line(u,8,"[ SELECTED SURFACE ]");line(u,11,"[ EXPORT SELECTED ASSETS ]");line(u,14,"[ ADVANCED - PIPELINE EXPLORER / SETTINGS ]");
        line(u,17,in.geometryMode?"[ PICK MODE: GEOMETRY CANDIDATES ]":"[ PICK MODE: LAST SCENE PASS ]");
        line(u,19,"CLICK THE WORLD OUTSIDE THIS PANEL TO PICK");line(u,20,"OR USE INSPECT SURFACE FOR A SMALLER PANEL");
        line(u,22,"[ SCENE DEBUGGER - RANKED FINDINGS ]");line(u,24,"[ SHADOWS AND OCCLUSION ]");
        line(u,23,in.reason);line(u,26,"[ WATER REFLECTIONS - SSR / PLANAR ]");
        line(u,28,"[ VOLUMETRIC FOG ]");
    } else if(in.page==1) {
        line(u,2,"INSPECT MODE - CLICK ANY VISIBLE SURFACE");line(u,4,"ESC / F10 - EXIT");
    } else if(in.page==2) {
        if(in.selected) {   // semantic class (draw_semantics.hpp) - why, on the next line
            const auto sem=classify_snapshot(s,in.selected->draw);
            line(u,3,std::string("CLASS: ")+draw_class_name(sem.cls)+" ("+draw_confidence_name(sem.confidence)+")");
            line(u,4,sem.reason);
        } else line(u,3,"SELECTED SURFACE - CLASSIFICATION: UNKNOWN");
        line(u,5,"SCREEN: "+std::to_string(in.x)+", "+std::to_string(in.y));
        line(u,6,"DEPTH: "+(in.depthReady?number(in.depth):"UNAVAILABLE / PENDING"));
        line(u,7,"VIEW POS / NORMAL: UNAVAILABLE");line(u,8,in.reason);
        if(!in.geometryCandidates.empty()) {
            line(u,21,"[ NEXT GPU HIT ] "+std::to_string(in.candidateIndex+1)+" / "+std::to_string(in.geometryCandidates.size())+(in.candidateLimit?" (LATEST 64)":""));
            line(u,22,"[ PREVIOUS GPU HIT ] - DRAW ORDER");
        }
        if(in.selected) {
            const auto& d=*in.selected;const auto& p=*d.draw.pipeline;
            line(u,10,std::string(in.geometryCandidate?"GEOMETRY CANDIDATE: ":in.finalVisible?"VISIBLE DRAW: ":"SCENE DRAW: ")+hex(in.drawIdentity));line(u,11,std::string(in.finalVisible?"PIPELINE: ":"SCENE PIPELINE: ")+std::to_string(p.serial)+"  FP "+hex(p.hash));
            line(u,12,"VS: "+identity(p.vs?p.vs->hash:0));line(u,13,"FS: "+identity(p.fs?p.fs->hash:0));
            line(u,14,"MATERIAL CANDIDATE: "+identity(in.materialIdentity));line(u,15,"MESH CANDIDATE: "+hex(in.meshIdentity));
            auto e=effective_state(d.draw);line(u,16,"DEPTH WRITE "+std::to_string(e.depthWrite)+" BLEND "+std::to_string(e.blend)+" CULL "+std::to_string(e.cull)+" TOPO "+std::to_string(e.topology));
            line(u,17,"TARGET "+std::to_string(d.draw.target.width)+"X"+std::to_string(d.draw.target.height)+" TEX "+std::to_string(textures(in).size()));
            unsigned buffers=0;for(const auto& b:d.draw.vertices)buffers+=b.buffer!=0;
            line(u,18,"VERTEX BUFFERS "+std::to_string(buffers)+" INDEX "+hex(d.draw.index.buffer));
            line(u,19,in.finalVisible?"FRONT-MOST AT PRESENT IMAGE: PROVEN":"FINAL UI / COMPOSITOR: VISIBLE ID UNKNOWN");
            line(u,20,in.geometryCandidate?"COVERED AT DRAW TIME / MAY BE OCCLUDED":"IDENTITIES ARE SESSION / GENERATION BASED");
        } else line(u,11,"DRAW / PIPELINE / MATERIAL: UNKNOWN");
        const bool canHighlight=can_highlight(in);
        line(u,23,!canHighlight?"[ HIGHLIGHT UNAVAILABLE FOR THIS DRAW ]":in.highlight?"[ HIGHLIGHT: ON - MATCHING DRAW CANDIDATE ]":"[ HIGHLIGHT: OFF ]");
        line(u,25,"[ INSPECT TEXTURES / BUFFERS ]");line(u,27,"[ INSPECT SHADERS ]");line(u,29,"[ PICK ANOTHER SURFACE ]");
    } else if(in.page==3) {
        auto list=textures(in);line(u,3,"TEXTURES - METADATA ID, CONTENT HASH: N/A");
        if(list.empty())line(u,6,"NO RESOLVED TEXTURES FOR SELECTED DRAW");
        else {
            in.texture%=list.size();auto& r=*list[in.texture];const auto& v=*r.descriptor.view;const auto& im=*v.image;
            line(u,4,"[ NEXT TEXTURE ] "+std::to_string(in.texture+1)+" / "+std::to_string(list.size()));
            line(u,6,"SET "+std::to_string(r.set)+" BIND "+std::to_string(r.binding)+" ELEMENT "+std::to_string(r.element));
            line(u,7,"IMAGE "+hex(im.handle)+" VIEW "+hex(v.handle));line(u,8,"TEXTURE ID "+hex(texture_identity(v)));
            line(u,9,"FORMAT "+inspector_format(v.format)+" SIZE "+std::to_string(im.extent.width)+"X"+std::to_string(im.extent.height));
            line(u,10,"MIPS "+std::to_string(im.mips)+" LAYERS "+std::to_string(im.layers)+" BASE MIP "+std::to_string(v.range.baseMipLevel));
            line(u,11,(im.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)?"RENDER-TARGET CAPABLE / MAY CONTAIN SCENE":"SAMPLED IMAGE / DIFFUSE-NORMAL ROLE UNKNOWN");
            if(auto sam=r.descriptor.sampler) {
                line(u,12,"SAMPLER "+hex(sam->handle));line(u,13,"MIN "+filter_name(sam->min)+" MAG "+filter_name(sam->mag)+" MIP "+std::to_string(sam->mipmap));
                line(u,14,"ADDR "+address_name(sam->address[0])+" / "+address_name(sam->address[1])+" / "+address_name(sam->address[2]));
            } else line(u,12,"SAMPLER: SEPARATE BINDING OR UNKNOWN");
            line(u,16,in.preview?"[ PREVIEW REQUESTED ]":"[ PREVIEW TEXTURE ]");
            line(u,17,in.previewStatus);
        }
        unsigned row=19;if(in.selected) {
            if(!in.selected->uniforms.empty())line(u,18,"M4:TAG% = MATH-ONLY MATRIX HINT, SEE EXPORT");
            unsigned binding=0;for(auto& b:in.selected->draw.vertices){if(b.buffer && row<21)line(u,row++,"VB"+std::to_string(binding)+" "+hex(b.buffer)+" +"+std::to_string(b.offset));++binding;}
            auto& index=in.selected->draw.index;if(index.buffer)line(u,row++,"INDEX "+hex(index.buffer)+" +"+std::to_string(index.offset));
            for(auto& r:in.selected->resources)if(r.descriptor.buffer && row<27)line(u,row++,"BUFFER S"+std::to_string(r.set)+" B"+std::to_string(r.binding)+" "+hex(r.descriptor.buffer)+" +"+std::to_string(r.descriptor.offset)+matrix_hint(in,r.set,r.binding,r.element));
        }
        line(u,28,"[ EXPORT OPTIONS - BITMAP / SHADERS ]");
        line(u,29,"[ BACK TO SELECTED SURFACE / GPU HITS ]");
    } else if(in.page==4) {
        line(u,3,"SHADER SNAPSHOTS SURVIVE MODULE DESTRUCTION");
        if(in.selected) {
            auto& p=*in.selected->draw.pipeline;unsigned row=5;
            unsigned stage=0;for(auto shader:{p.vs,p.fs}) {
                line(u,row++,stage==0?"VERTEX SHADER":"FRAGMENT SHADER");
                line(u,row++,"HASH "+identity(shader?shader->hash:0));
                line(u,row++,"SOURCE "+(stage==0?p.vsSource:p.fsSource));
                line(u,row++,"ENTRY "+(stage==0?p.vsEntry:p.fsEntry));
                if(shader){line(u,row++,"SPIR-V BYTES "+std::to_string(shader->words.size()*4));line(u,row++,"BINDINGS "+std::to_string(shader->bindings.size())+" SAMPLES "+std::to_string(shader->samples));}
                row+=2;++stage;
            }
        } else line(u,6,"NO SELECTED DRAW / SHADER UNKNOWN");
        line(u,25,"[ EXPORT VS + FS SPIR-V ]");line(u,27,asset_export_status(in.assetExport));
    } else if(in.page==7) {
        auto& water=water_state(device_state(device));
        line(u,3,"EXPERIMENTAL - FLAT WOW WATER PROFILE");
        line(u,5,"[ USE SELECTED WATER CANDIDATE ]");
        line(u,6,water.selectedVS?"WATER PROFILE SELECTED":"PICK WATER IN GEOMETRY MODE FIRST");
        line(u,8,std::string("[ MODE: ")+(water.mode==0?"OFF":water.mode==1?"SSR":"PLANAR")+" ]");
        line(u,11,std::string("[ VIEW: ")+waterDebugViewNames[water.debug<waterDebugViewCount?water.debug:0]+" ]");
        line(u,14,"[ STRENGTH: "+std::to_string(int(water.strength*100+.5f))+"% ]");
        line(u,17,water.status);line(u,19,"PASSES: "+std::to_string(water.passes)+"  CAPTURES: "+std::to_string(water.captureCount));
        line(u,20,"[ CAPTURE DEBUG DATA ]");
        line(u,21,std::string("[ STYLE: ")+(water.style?"MODERN":"CLASSIC")+" - A/B ]");
        line(u,22,"PLANAR: SAME-PASS OPAQUE DRAWS");
        line(u,23,std::string("[ LAYERS: ")+(water.layers==waterLayersAll?"ALL":water.layers==waterLayerReflection?"REFLECTION":water.layers==(waterLayerReflection|waterLayerRefraction)?"REFLECTION+WAVES":water.layers==(waterLayerReflection|waterLayerDepthTint|waterLayerFoam)?"REFLECTION+DEPTH+FOAM":water.layers==(waterLayerRefraction|waterLayerDepthTint|waterLayerFoam)?"NO REFLECTION":"CUSTOM")+" ]");
        line(u,25,std::string("[ TONE: ")+waterTones[water.tone<waterToneCount?water.tone:0].name+" ]");
        line(u,26,"[ WATER DETAILS: FOAM, TEXTURE, QUALITY ]");
        line(u,27,"[ PERFORMANCE TEST: STAND STILL 40 S ]");
        line(u,28,"SETTINGS SAVE AUTOMATICALLY");line(u,29,"F10 CLOSES MENU; MODE OFF STOPS REFLECTIONS");
    } else if(in.page==12) {
        auto& state=device_state(device);
        line(u,1,"SAVED IN FOG.INI; CHANGES SHOW AT ONCE");
        line(u,2,"SHADOWS AND OCCLUSION - CLICK A ROW TO CHANGE IT");
        for(unsigned i=0;i<shade_menu_rows();++i){line(u,4+2*i,"[ "+shade_menu_label(state,i)+" ]");line(u,5+2*i,shade_menu_hint(i));}   // (rows 4..29: 13 settings and their hints)
    } else if(in.page==11) {
        auto& state=device_state(device);
        line(u,3,"VOLUMETRIC FOG - CLICK A ROW TO CHANGE IT");
        // Rows 4..29 (26 at most) - row 30 is BACK, 31 the input hint. The status goes to row 1: on row 29 it covered RESET TO
        // DEFAULTS once LIGHT SOURCES made the rows 25 (2026-09-28) - a click on the status line reset every setting.
        for(unsigned i=0;i<fog_menu_rows() && 4+i<30;++i)line(u,4+i,"[ "+fog_menu_label(state,i)+" ]");
        const auto& f=fog_state(state);
        char buf[96];std::snprintf(buf,sizeof buf,"BASE Z %.1f - SHIFT+F5 VIEWS - ROW 30 BACK",double(f.lastBaseHeight));
        line(u,1,buf);
    } else if(in.page==8) {
        auto& water=water_state(device_state(device));
        line(u,3,"WATER DETAILS - CLICK A ROW TO CHANGE IT");
        const unsigned n=unsigned(waterLookOptionCount);
        for(unsigned i=0;i<n;++i)line(u,5+i,"[ "+water_look_label(water.look,i)+" ]");
        line(u,5+n,std::string("[ REFLECTION QUALITY: ")+waterQualityNames[water.look.quality<waterQualityCount?water.look.quality:1]+" ]");
        line(u,7+n,"[ RESET DETAILS TO DEFAULT ]");
        line(u,9+n,"GAME TEXTURE OPACITY: 0 = ONLY OUR WATER LOOK");
        line(u,10+n,"CLARITY/CAUSTICS NEED THE DEPTH LAYER; GLITTER");
        line(u,11+n,"THE REFLECTION LAYER; WHITECAPS THE FOAM LAYER");
        line(u,28,"SETTINGS SAVE AUTOMATICALLY");line(u,29,"ROW 30 GOES BACK");
    } else if(in.page==5) {
        line(u,3,"EXPORT SELECTED DRAW ASSETS");
        line(u,5,in.selected?"BITMAPS KEEP SIZE AND ALPHA / SELECTED MIP":"SELECT A SURFACE FIRST");
        line(u,8,"[ EXPORT THIS TEXTURE AS BMP ]");
        line(u,11,"[ EXPORT ALL TEXTURES + SHADERS ]");
        line(u,14,"[ EXPORT VS + FS SPIR-V ]");
        line(u,17,asset_export_status(in.assetExport));
        line(u,19,"DIFFUSE / NORMAL ROLE: UNKNOWN");
        line(u,20,"FILES ARE NAMED BY SET / BINDING / ID");
        if(in.assetExport) {
            std::lock_guard lock(in.assetExport->lock);const auto& path=in.assetExport->directory;
            line(u,22,"OUTPUT DIRECTORY:");
            for(size_t n=0;n<5 && n*48<path.size();++n)line(u,23+n,path.substr(n*48,48));
        }
        line(u,29,"MANIFEST LISTS SAVED / UNAVAILABLE ASSETS");
    } else if(in.page==9) {
        line(u,0,"SCENE DEBUGGER - OVERVIEW");
        line(u,4,"FRAME "+std::to_string(s.frameSerial));
        auto* sd=s.sceneDebugger.get();
        const CameraCandidate* topCam=sd && !sd->cameras.empty()?&sd->cameras.front():nullptr;
        line(u,5,"DRAWS OBSERVED LAST FRAME: "+std::to_string(sd?sd->totalDrawsLastFrame:0));
        line(u,7,"CAMERA");
        line(u,8,topCam?"["+finding_status(true,topCam->confidence,topCam->verdict)+"] #1 - "+std::to_string(int(topCam->confidence*100+.5f))+"%":"[ UNKNOWN ] OPEN CAMERA CANDIDATES AND MOVE AROUND");
        line(u,10,"DEPTH");
        line(u,11,in.depthImage?"[ LIKELY ] "+std::to_string(in.width)+"X"+std::to_string(in.height)+" (SCENE-DEPTH SELECTOR)":"[ UNKNOWN ]");
        // Temporary diagnostic panel (2026-09): pinpoints exactly where the
        // water-anchor pipeline stops, replacing the SUN/SHADOW/FOG/world-
        // reconstruction placeholders on this page until it's confirmed
        // working in real WoW at least once. Those placeholders carried no
        // information beyond "not implemented" anyway.
        {
            const WaterCameraAnchor empty{};
            const auto& wa=sd?sd->waterAnchor:empty;
            line(u,13,"WATER CAMERA PIPELINE");
            line(u,14,std::string("VS:")+(wa.vsSeen?"Y":"N")+" FS:"+(wa.fsSeen?"Y":"N")+" S2/B0:"+(wa.bindingFound?"Y":"N")+" RANGE:"+std::to_string(wa.bufferRange)+"B");
            line(u,15,"HOST CAPTURE: "+wa.hostCaptureStatus);
            line(u,16,"GPU READBACK: "+wa.gpuReadbackStatus);
            line(u,17,"[ CAMERA CANDIDATES ]");
            line(u,18,"BYTES:"+std::to_string(wa.bytesCaptured)+" P+256:"+(wa.projAvailable?"OK":"MISSING")+" V+336:"+(wa.viewAvailable?"OK":"MISSING"));
            line(u,19,"[ RAW / ADVANCED INSPECTOR ]");
            line(u,20,"CAMERA MATH: "+wa.cameraMathStatus);
            line(u,21,std::string("FINAL CAMERA: ")+(wa.valid?"OK (SEE CAMERA CANDIDATES)":"NONE"));
            unsigned row=22;wrap_lines(u,row,24,"LAST ERROR: "+water_last_error(wa));
        }
        if(in.selected)line(u,25,"SELECTED (RAW INSPECTOR): "+hex(in.drawIdentity));
        {
            DerivedCamera active;
            const bool haveActive=sd && active_scene_camera(sd->cameras,active);
            line(u,26,haveActive?"ACTIVE SCENE CAMERA: OK - SEE CAMERA CANDIDATES #1":"ACTIVE SCENE CAMERA: NONE");
        }
        line(u,27,"NEXT ACTION: LOOK AT VISIBLE WATER WITH THIS PAGE OPEN");
    } else if(in.page==10) {
        line(u,0,"SCENE DEBUGGER - CAMERA CANDIDATES");
        line(u,2,"CAPTURING WHILE THIS PAGE STAYS OPEN");
        auto& sd=scene_debugger_state(s);
        if(sd.cameras.empty()) {
            line(u,4,"NO CANDIDATES YET");
            line(u,5,"MOVE / LOOK AROUND FOR A FEW SECONDS");
        } else {
            if(sd.cameraIndex>=sd.cameras.size())sd.cameraIndex=0;
            const auto& c=sd.cameras[sd.cameraIndex];
            line(u,4,"#"+std::to_string(sd.cameraIndex+1)+" / "+std::to_string(sd.cameras.size())+"   "+std::to_string(int(c.confidence*100+.5f))+"%   "+finding_status(true,c.confidence,c.verdict));
            const bool fromMemory=c.hasDerivedCamera && c.derivedCamera.source=="wow-memory-cgcamera";
            line(u,5,fromMemory?"SOURCE: WOW PROCESS MEMORY - CGCAMERA":"SHADER VS "+hex(c.vsHash));
            if(fromMemory)line(u,6,"PROJ/VIEW: BUILT FROM THE MEMORY STRUCT");
            else line(u,6,c.hasProj?"PROJ: SET"+std::to_string(c.projSet)+" B"+std::to_string(c.projBinding)+" +"+std::to_string(c.projOffset):"PROJECTION: NOT FOUND");
            if(!fromMemory)line(u,7,c.hasView?"VIEW: SET"+std::to_string(c.viewSet)+" B"+std::to_string(c.viewBinding)+" +"+std::to_string(c.viewOffset):"VIEW: NOT FOUND");
            line(u,8,"SEEN "+std::to_string(c.framesObserved)+" FRAMES, "+std::to_string(c.drawsObserved)+" DRAWS");
            line(u,9,level_label(c.level));
            line(u,10,"EVIDENCE:");
            unsigned row=11;
            for(auto& e:c.evidence)wrap_lines(u,row,20,e);
        }
        line(u,22,"[ NEXT CANDIDATE ]");line(u,23,"[ PREVIOUS CANDIDATE ]");
        line(u,25,"[ ACCEPT AS CAMERA ]");line(u,26,"[ REJECT CANDIDATE ]");
        line(u,28,"[ OVERVIEW ]");line(u,29,"[ RAW / ADVANCED ]");
    }
}
}
