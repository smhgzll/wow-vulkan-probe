#include "overlay_menu.hpp"
#include "layer_state.hpp"
#include "overlay_uniform.hpp"
#include "pipeline_discovery.hpp"
#include "surface_inspector.hpp"
#include <algorithm>
#include <cstdlib>
namespace depth_probe {
uint32_t menu_page_count() { return discovery_first_page()+2; }
MenuRect menu_control_rect(unsigned control, unsigned page) {
    if (control == 14) return {316, 22, 60, 24};
    if (page && control < 10) return {24, 78.f+44.f*control, 352, 38};
    if (page && control == 10) return {0,0,0,0};
    if (control == 13) return {24, 538, 352, 16};
    if (control == 0) return {24, 78, 352, 28};
    if (control <= 6) return {24, (control <= 3 ? 148.f : 316.f) + ((control - 1) % 3) * 44.f, 352, 38};
    if (control <= 10) return {24 + ((control - 7) % 2) * 184.f, 478 + ((control - 7) / 2) * 32.f, 168, 26};
    return {24 + (control - 11) * 184.f, 556, 168, 32};
}
float menu_scale(uint32_t width, uint32_t height) {
    return std::min({1.f, width / (menu_width + 2 * menu_margin), height / (menu_height + 2 * menu_margin)});
}
static constexpr float lows[] = {0, 1, 0, 0, .1f, 0};
static constexpr float highs[] = {3, 32, .02f, 2, 10, 1};
static float& slider_field(OverlaySettings& s, unsigned i) {
    switch (i) {
    case 0: return s.aoStrength;
    case 1: return s.aoRadius;
    case 2: return s.aoBias;
    case 3: return s.rainbowStrength;
    case 4: return s.hueFrequency;
    default: return s.opacity;
    }
}
float menu_slider_value(const OverlaySettings& s, unsigned i) {
    if (i >= 6) return 0;
    auto copy = s;
    return (slider_field(copy, i) - lows[i]) / (highs[i] - lows[i]);
}
void menu_set_slider(OverlaySettings& s, unsigned i, float fraction) {
    if (i < 6) slider_field(s, i) = lows[i] + std::clamp(fraction, 0.f, 1.f) * (highs[i] - lows[i]);
}
void OverlayMenu::input(const MenuInput& in) {
    pointerX = in.x; pointerY = in.y; pointerValid = in.pointerValid && in.focused;
    const bool previous = visible;
    const bool edge = in.f10 && !f10WasDown;
    f10WasDown = in.f10;
    if (!in.focused) { visible = false; dragging = -1; }
    else if (edge) {
        visible = !visible;
        log_line("[UI-HOTKEY] key=F10 visible=" + std::to_string(visible));
    }
    if (in.key == MenuKey::Escape) visible = false;
    if (previous != visible) log_line("[UI-VISIBLE] visible=" + std::to_string(visible));
    if (previous && !visible) stop_discovery_isolation();
    const bool click = in.leftDown && !leftWasDown;
    leftWasDown = in.leftDown;
    if (inspector_menu_input(*this,in,click))return;
    if (!visible) { dragging = -1; return; }
    if (in.key == MenuKey::Up) selected = (selected + 14) % 15;
    if (in.key == MenuKey::Down) selected = (selected + 1) % 15;
    bool activate = in.key == MenuKey::Activate;
    if (click && in.pointerValid) {
        for (unsigned i = 0; i < 15; ++i) {
            const auto r = menu_control_rect(i, page);
            if (in.x >= r.x && in.x <= r.x + r.w && in.y >= r.y && in.y < r.y + r.h) {
                selected = i; activate = true;
                if ((page && i < 10) || (!page && i >= 1 && i <= 6)) dragging = static_cast<int>(i);
                break;
            }
        }
    }
    if (activate && selected == 14) { page=(page+1)%menu_page_count(); dragging=-1; return; }
    if (page && activate && selected == 13) { --page; dragging=-1; return; }
    if (discovery_menu_input(page,selected,in.key==MenuKey::Left?-1:in.key==MenuKey::Right?1:0,activate)) {dragging=-1;return;}
    auto settings = overlay_settings();
    const auto before = settings;
    if (page) {
        const auto& controls=settings_controls();
        const auto row=(page-1)*10+selected;
        if(selected<10 && row<controls.size()) {
            const auto& c=controls[row];
            if(dragging>=0 && in.leftDown && in.pointerValid && c.floating)
                set_setting_fraction(settings,c,(in.x-24)/240);
            if(in.key==MenuKey::Left || in.key==MenuKey::Right) step_setting(settings,c,in.key==MenuKey::Left?-1:1);
            if(activate && c.integer) {
                auto& value=settings.*c.integer;
                value=value>=uint32_t(c.high)?uint32_t(c.low):value+1;
            }
        }
        if(!in.leftDown)dragging=-1;
        if(activate && selected==11) status=save_overlay_settings()?1:2;
        if(activate && selected==12) {reset_overlay_settings();status=3;return;}
        if(!(settings==before)){set_overlay_settings(settings);status=0;}
        return;
    }
    if (dragging >= 1 && in.leftDown && in.pointerValid)
        menu_set_slider(settings, dragging - 1, (in.x - 24) / 352);
    if (!in.leftDown) dragging = -1;
    if (selected >= 1 && selected <= 6 && (in.key == MenuKey::Left || in.key == MenuKey::Right)) {
        const auto slider = selected - 1;
        menu_set_slider(settings, slider, menu_slider_value(settings, slider) + (in.key == MenuKey::Left ? -.01f : .01f));
    }
    if (activate) {
        if (selected == 13) settings.debugMode = settings.debugMode == 4 ? 5 : 4;
        else if (selected == 0) settings.effectEnabled ^= 1;
        else if (selected >= 7 && selected <= 10) settings.debugMode = selected - 7;
        else if (selected == 11) status = save_overlay_settings() ? 1 : 2;
        else if (selected == 12) { reset_overlay_settings(); status = 3; return; }
    }
    if (!(settings == before)) { set_overlay_settings(settings); status = 0; }
}
void populate_menu_uniform(OverlayUniform& value) {
    std::memset(value.rows,0,sizeof(value.rows));value.panel[3]=0;
    const auto page=overlay_menu().page;
    value.panel[0]=page;value.panel[1]=menu_page_count();
    const auto& s=value.settings;
    value.panel[2]=s.depthConvention && s.projectionMode && s.nearPlane>=.0001f && s.farPlane>s.nearPlane &&
        (s.projectionMode==1?s.verticalFov>0:s.orthoHeight>0);
    if(!page)return;
    const auto& controls=settings_controls();
    for(uint32_t i=0;i<10;++i) {
        const auto index=(page-1)*10+i;if(index>=controls.size())break;
        const auto& c=controls[index];auto& row=value.rows[i];
        row[0]=setting_value(s,c);row[1]=setting_fraction(s,c);row[2]=float(setting_label(c.key));
        row[3]=c.floating?2.f:c.high==1?1.f:3.f;
    }
}
OverlayMenu& overlay_menu() {
    static OverlayMenu menu = [] {
        OverlayMenu result;
        const char* visible = std::getenv("DEPTH_PROBE_MENU_VISIBLE");
        result.visible = visible && std::string(visible) == "1";
        log_line("[UI-VISIBLE] visible=" + std::to_string(result.visible));
        return result;
    }();
    return menu;
}
} // namespace depth_probe
