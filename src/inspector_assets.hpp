#pragma once
#include "discovery_internal.hpp"

namespace depth_probe::discovery {
struct Inspector;
constexpr uint64_t maxTextureExportBytes=64ull*1024*1024;
struct TextureSource {
    std::shared_ptr<View> view;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width=0,height=0,conversion=0;
    std::string reason;
    explicit operator bool()const{return view && reason.empty();}
};
TextureSource inspector_texture_source(const Device&,const ResourceUse&,uint64_t queue);
struct TextureAsset {
    ResourceUse resource;
    uint32_t width=0,height=0;
    std::string file,status="queued",reason;
};
struct AssetExport {
    std::mutex lock;
    std::string directory,message="Preparing export",draw,scope,drawState;
    std::vector<TextureAsset> textures;
    std::vector<std::shared_ptr<const UniformSnapshot>> uniforms;
    std::shared_ptr<Shader> vs,fs;
    std::string vsEntry,fsEntry;
    bool ready=false,failed=false;
};
void request_asset_export(Inspector&,bool all,bool shadersOnly=false);
void auto_export_uniforms(Inspector&);
bool asset_export_busy(const std::shared_ptr<AssetExport>&);
std::string asset_export_status(const std::shared_ptr<AssetExport>&);
void asset_texture_failed(const std::shared_ptr<AssetExport>&,size_t,const std::string&);
bool save_asset_pixels(const std::shared_ptr<AssetExport>&,size_t,std::vector<uint8_t>&&);
void flush_asset_exports();
}
