#include "overlay_menu.hpp"
#include "surface_inspector.hpp"
#include "overlay_internal.hpp"
#include "fake_xcb.hpp"
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace depth_probe;
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();} }while(0)
static void frames(unsigned milliseconds=90) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    do {poll_menu_input(1920,1080);std::this_thread::sleep_for(std::chrono::milliseconds(3));}while(std::chrono::steady_clock::now()<end);
}
static void click(float x,float y) {
    test_xcb_pointer(x+menu_margin,y+menu_margin,true);frames();
    test_xcb_pointer(x+menu_margin,y+menu_margin,false);frames();
}
int main(int argc,char** argv) {
    const std::string root="/tmp/depth-probe-input-backend-"+std::to_string(getpid());
    setenv("XDG_CONFIG_HOME",root.c_str(),1);setenv("DEPTH_PROBE_LAYER_LOG",(root+".log").c_str(),1);
    setenv("DISPLAY",":depth-probe-fake",1);unsetenv("DEPTH_PROBE_UI_INPUT");unsetenv("DEPTH_PROBE_MENU_VISIBLE");
    const unsigned mode=argc>1?std::strtoul(argv[1],nullptr,10):0;
    const bool inspector=argc>2 && std::string(argv[2])=="inspector";
    if(inspector)setenv("DEPTH_PROBE_WATER_SETTINGS","/dev/null/depth-probe-water.ini",1); /* tests never read or write the user's water settings */
    setenv("DEPTH_PROBE_SURFACE_INSPECTOR","1",1);
    const bool sharedPointer = argc <= 2 || inspector;
    setenv("DEPTH_PROBE_UI_SHARED_POINTER", sharedPointer ? "1" : "0", 1);
    test_xcb_grab_status(mode&1?0:1,mode&2?0:1);
    if(inspector) {
        auto device=reinterpret_cast<VkDevice>(uintptr_t(1));
        DepthOverlayState context;context.width=1920;context.height=1080;
        context.depthImage=(VkImage)uint64_t(2);context.depthGeneration=1;
        inspector_context(device,context);
        auto& state=*discovery::devices.at(device).inspector;
        frames();test_xcb_key(10,true);frames();test_xcb_key(10,false);frames();
        CHECK(overlay_menu().visible && state.page==0);
        click(60,126);CHECK(state.page==1); // Actual home button through the XCB worker.
        OverlayUniform u{};populate_inspector_menu(device,u);
        const auto* text=reinterpret_cast<const char*>(u.inspectorText);
        CHECK(std::string(text+2*48,12)=="INSPECT MODE");
        click(850,480);CHECK(state.ticket==1 && state.x==866 && state.y==496 && state.armed);
        test_xcb_pointer(916,516,true);frames(200);CHECK(state.ticket==2); // Selected panel permits world re-pick.
        test_xcb_pointer(916,516,false);frames();
        click(50,32);CHECK(state.ticket==2); // Panel labels never select the world underneath.
        test_xcb_key(11,true);if(mode&1)test_xcb_key_event(11);frames();test_xcb_key(11,false);frames();
        CHECK(!overlay_menu().visible && !state.armed && !state.highlight);
        click(850,480);CHECK(state.ticket==2); // Menu closed: game input unaffected.
        test_xcb_key(10,true);frames();test_xcb_key(10,false);frames();
        click(900,500);CHECK(state.ticket==3); // World click from home works too.
        if(mode&2) {
            test_xcb_button_event(true,1016,616);test_xcb_button_event(false,1016,616);frames();
            CHECK(state.ticket==4 && state.x==1016 && state.y==616);
        }
        release_menu_input();destroy_device_discovery(device);
        std::printf("PASS: Inspector real input worker: mode=%u, F10/home/inspect, world click/re-pick, held/fast click, panel exclusion, Escape\n",mode);
        return 0;
    }
    frames();CHECK(test_xcb_queries()>0);
    test_xcb_key(10,true);frames();CHECK(overlay_menu().visible);
    frames();CHECK(overlay_menu().visible);CHECK(test_xcb_grabs()==0); // F10 still held
    test_xcb_key(10,false);frames();
    const auto enabled=overlay_settings().effectEnabled;
    click(60,90);
    CHECK((overlay_settings().effectEnabled!=enabled) == (sharedPointer || (mode&2))); // reported failure
    CHECK(menu_input_available() == (mode==3 ? 1u : 2u));
    CHECK(test_xcb_grabs()==mode); // one successful grab survives the other failing
    const auto attempts = test_xcb_attempts();
    frames(); CHECK(test_xcb_attempts() == attempts); // no grab retry loop
    auto key = [&](uint8_t code) {
        test_xcb_key(code,true); if(mode&1)test_xcb_key_event(code); frames();
        test_xcb_key(code,false); frames();
    };
    overlay_menu().selected=0;
    key(14); CHECK(overlay_menu().selected==1); // Down
    key(16); CHECK(overlay_settings().aoStrength>1); // Right
    overlay_menu().selected=0;
    const auto beforeKey=overlay_settings().effectEnabled;
    test_xcb_key(17,true); if(mode&1)test_xcb_key_event(17); frames(200);
    CHECK(overlay_settings().effectEnabled!=beforeKey); // held Enter toggles only once
    test_xcb_key(17,false);frames();
    if (!sharedPointer && !(mode&2)) {
        key(11);CHECK(!overlay_menu().visible);
        release_menu_input();CHECK(test_xcb_grabs()==0);
        std::filesystem::remove_all(root);
        std::puts("PASS: shared-pointer opt-out keeps keyboard navigation, no pointer actions");
        return 0;
    }
    if(mode&2) {
        // Fast press+release entirely between pointer snapshots is not dropped.
        const auto prior=overlay_settings().effectEnabled;
        test_xcb_button_event(true,76,106); test_xcb_button_event(false,76,106); frames();
        CHECK(overlay_settings().effectEnabled!=prior);
    }
    const auto slider=menu_control_rect(1);
    test_xcb_pointer(slider.x+menu_margin,slider.y+menu_margin+25,true);frames();
    test_xcb_pointer(slider.x+slider.w+menu_margin,slider.y+menu_margin+25,true);frames();
    CHECK(overlay_settings().aoStrength==3);
    test_xcb_pointer(0,0,false);frames();
    click(250,568);CHECK(overlay_settings()==OverlaySettings{}); // Reset
    click(60,568);CHECK(std::filesystem::exists(settings_config_path())); // Save
    test_xcb_focus(false);frames();CHECK(!overlay_menu().visible && test_xcb_grabs()==0);
    const auto old=overlay_settings();const auto queries=test_xcb_queries();
    click(60,90);CHECK(overlay_settings()==old && test_xcb_queries()==queries);
    test_xcb_focus(true);frames();
    test_xcb_key(10,true);frames();test_xcb_key(10,false);frames();CHECK(overlay_menu().visible);
    test_xcb_key(11,true);if(mode&1)test_xcb_key_event(11);frames();
    test_xcb_key(11,false);frames();CHECK(!overlay_menu().visible && test_xcb_grabs()==0);
    overlay_menu().visible=true;frames();
    std::this_thread::sleep_for(std::chrono::milliseconds(650)); // renderer stalled
    CHECK(test_xcb_grabs()==0 && menu_input_available()==0);
    const auto paused=overlay_settings();
    test_xcb_pointer(76,106,true);frames();CHECK(overlay_settings()==paused && test_xcb_grabs()==0);
    test_xcb_pointer(76,106,false);frames();CHECK(overlay_settings()==paused && test_xcb_grabs()==mode);
    release_menu_input();CHECK(test_xcb_grabs()==0);
    std::filesystem::remove_all(root);
    std::printf("PASS: production XCB worker with fake server: grab mask=%u, F10, mouse buttons/drag/Save/Reset, focus loss, Escape, cleanup\n",mode);
}
