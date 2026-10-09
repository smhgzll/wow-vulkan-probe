#include "water_reflections.hpp"
#include <dlfcn.h>
#include <fstream>
#include <cstdio>
using namespace depth_probe::discovery;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static std::vector<uint32_t> read(const char* path){std::ifstream file(path,std::ios::binary|std::ios::ate);CHECK(file);auto size=file.tellg();CHECK(size>0 && size%4==0);std::vector<uint32_t> words(size_t(size)/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),size);return words;}
int main(int argc,char** argv) {
    void* lib=dlopen("libSPIRV-Tools-shared.so",RTLD_NOW|RTLD_LOCAL);
    auto create=lib?reinterpret_cast<void*(*)(int)>(dlsym(lib,"spvContextCreate")):nullptr;
    auto destroy=lib?reinterpret_cast<void(*)(void*)>(dlsym(lib,"spvContextDestroy")):nullptr;
    auto validate=lib?reinterpret_cast<int(*)(void*,const uint32_t*,size_t,void**)>(dlsym(lib,"spvValidateBinary")):nullptr;
    auto print=lib?reinterpret_cast<int(*)(void*)>(dlsym(lib,"spvDiagnosticPrint")):nullptr;
    auto freeDiagnostic=lib?reinterpret_cast<void(*)(void*)>(dlsym(lib,"spvDiagnosticDestroy")):nullptr;
    // SPV_ENV_VULKAN_1_2, per the public SPIRV-Tools C ABI.
    void* context=create?create(23):nullptr;
    auto verify=[&](const auto& words){CHECK(!words.empty());if(context){void* diagnostic{};auto r=validate(context,words.data(),words.size(),&diagnostic);if(r)print(diagnostic);freeDiagnostic(diagnostic);CHECK(r==0);}};
    std::string reason;auto vs=read(DEPTH_PROBE_TEST_ROOT_DIR "/tests/water_scene.vert.spv"),fs=read(DEPTH_PROBE_TEST_ROOT_DIR "/tests/water_color.frag.spv");
    auto reflected=reflect_vertex_spirv(vs,"main",reason);verify(reflected);auto mask=water_fragment_spirv(fs,"main",reason);verify(mask);
    CHECK(reflect_vertex_spirv(vs,"missing",reason).empty());CHECK(water_fragment_spirv(fs,"missing",reason).empty());
    CHECK(water_fragment_spirv(vs,"main",reason).empty());CHECK(reflect_vertex_spirv(fs,"main",reason).empty());
    auto truncated=vs;truncated.pop_back();CHECK(reflect_vertex_spirv(truncated,"main",reason).empty());
    // Reject set 3 conflicts without turning an application descriptor into the
    // layer's private camera binding.
    auto conflict=vs;for(size_t i=5;i<conflict.size();i+=conflict[i]>>16)if((conflict[i]&65535)==71 && conflict[i+2]==34)conflict[i+3]=3;
    CHECK(reflect_vertex_spirv(conflict,"main",reason).empty());
    if(argc==3){verify(reflect_vertex_spirv(read(argv[1]),"main",reason));verify(water_fragment_spirv(read(argv[2]),"main",reason));std::puts("PASS: captured WoW VS and FS instrumentation validates");}
    if(context)destroy(context);if(lib)dlclose(lib);
    std::printf("PASS: vertex reflection, fragment mask preserving discard, invalid entry/module and reserved binding; SPIRV-Tools=%s\n",context?"validated":"not installed (GPU validation is separate)");
}
