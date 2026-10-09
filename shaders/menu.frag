#version 450
#extension GL_GOOGLE_include_directive : require
#include "overlay_uniform.glsl"
#include "menu_font.glsl"
layout(set=0,binding=0) uniform sampler2D diagnosticDepth;
layout(location = 0) in vec2 panel;
layout(location = 1) in flat uint isHud;
layout(location = 0) out vec4 outColor;
const vec3 ink = vec3(.87,.91,.97), muted = vec3(.48,.56,.68), accent = vec3(.28,.85,.77);
bool box(vec2 p, vec4 r) { return all(greaterThanEqual(p,r.xy)) && all(lessThan(p,r.xy+r.zw)); }
bool letter(vec2 p, uint c) {
    ivec2 q = ivec2(floor(p));
    if(q.x<0 || q.x>=5 || q.y<0 || q.y>=7)return false;
    return glyphBits(c,q.y*5+q.x)!=0u;
}
bool textAt(vec2 p, vec2 origin, int label, float scale) {
    vec2 q = (p-origin)/scale;
    if (q.x < 0 || q.y < 0 || q.y >= 7) return false;
    int col = int(q.x)/6;
    return letter(vec2(mod(q.x,6),q.y),labelChar(label,col));
}
bool numberAt(vec2 p, float value, int decimals) {
    vec2 q = (p-vec2(292,0))/2.;
    if (q.x < 0 || q.y < 0 || q.y >= 7) return false;
    int col = int(q.x)/6;
    // Two integer digits and a decimal point, followed by 2 or 4 decimals.
    int scaled = int(round(value*pow(10.,float(decimals))));
    uint c = 32u;
    if (col == 2) c = 46u;
    else if (col < 2) {
        int n = scaled / int(pow(10.,float(decimals))) / (col == 0 ? 10 : 1) % 10;
        if (col != 0 || n != 0) c = uint(n)+48u;
    } else if (col < 3+decimals) c = uint(scaled / int(pow(10.,float(decimals+2-col))) % 10)+48u;
    return letter(vec2(mod(q.x,6),q.y),c);
}
bool valueAt(vec2 p,vec2 origin,float value) {
    vec2 q=(p-origin)/1.25;
    if(q.x<0.0||q.y<0.0||q.y>=7.0)return false;
    int col=int(q.x)/6;
    if(col>13)return false;
    uint c=32u;
    const uint powers[7]=uint[](1000000u,100000u,10000u,1000u,100u,10u,1u);
    float safeValue=clamp(value,0.0,9999999.0);
    uint whole=uint(floor(safeValue));
    uint fractional=uint(clamp(round(fract(safeValue)*1000000.0),0.0,999999.0));
    if(col==7)c=46u;
    else if(col<7) {
        uint divisor=powers[col];
        if(col==6||whole>=divisor)c=48u+(whole/divisor)%10u;
    } else c=48u+(fractional/powers[col-7])%10u;
    return letter(vec2(mod(q.x,6.0),q.y),c);
}
bool hexAt(vec2 p,vec2 origin,int slot) {
    vec2 q=(p-origin)/1.5;
    if(q.x<0.0 || q.y<0.0 || q.y>=7.0 || q.x>=96.0)return false;
    int col=int(q.x)/6;
    int index=clamp(slot,0,3)*2+(col<8?1:0);
    uint word=fx.explorer[index/4][index%4];
    uint digit=(word>>uint((7-col%8)*4))&15u;
    return letter(vec2(mod(q.x,6.0),q.y),digit<10u?48u+digit:65u+digit-10u);
}
void inspectorMain(vec2 p) {
    bool inspecting=fx.inspector.y==1u;
    vec3 color=vec3(.04,.055,.08);
    float alpha=(!inspecting && box(p,vec4(0,0,400,648))) || (inspecting && box(p,vec4(0,0,400,124))) ? .98 : 0.;
    if(alpha>0.) {
        int row=int(floor((p.y-30.)/18.));
        bool button=false;
        if(row>=0 && row<32) {
            int firstWord=row*12;
            button=(fx.inspectorText[firstWord/4][firstWord%4]&255u)==91u;
            if(button && p.x>=12. && p.x<388.)color=vec3(.085,.17,.19);
        }
        vec2 q=(p-vec2(16.,30.+18.*float(row)))/1.25;
        int col=int(floor(q.x/6.));
        if(row>=0 && row<32 && col>=0 && col<48 && q.y>=0. && q.y<7.) {
            int byteIndex=row*48+col;
            int wordIndex=byteIndex/4;
            uint word=fx.inspectorText[wordIndex/4][wordIndex%4];
            uint c=(word>>uint((byteIndex%4)*8))&255u;
            if(letter(vec2(mod(q.x,6.),q.y),c))color=row==0 || button?accent:ink;
        }
        if(!inspecting && button && row==int(fx.menu.y) && p.x>=12. && p.x<388. && mod(p.y-30.,18.)>14.)color=accent;
    }
    vec2 pixel=(p+vec2(16))*fx.viewport.z;
    if(fx.inspector.z!=0u) {
        vec2 d=abs(pixel-vec2(fx.pick.xy)-vec2(.5));
        if((d.x<1. && d.y<9.) || (d.y<1. && d.x<9.)){color=fx.inspector.z==2u?vec3(1,1,0):vec3(1,.55,.1);alpha=1.;}
    }
    vec2 cursor=p-fx.pointer.xy;
    if(fx.pointer.z>0. && cursor.y>=0. && cursor.y<16. && cursor.x>=0. && cursor.x<cursor.y*.6+1.){color=ink;alpha=1.;}
    outColor=vec4(color,alpha);
}
// Read-only: no buttons, no highlight, no cursor - the HUD never accepts
// input (see camera_hud_input.hpp). Row 0 is drawn in the accent color as a
// title convention shared with inspectorMain.
void hudMain(vec2 p) {
    vec3 color=vec3(.03,.045,.065);
    float alpha=box(p,vec4(0,0,300,460))?.85:0.;
    if(alpha>0.) {
        int row=int(floor(p.y/16.));
        vec2 q=(p-vec2(8.,2.+16.*float(row)));
        int col=int(floor(q.x/6.));
        if(row>=0 && row<24 && col>=0 && col<48 && q.y>=0. && q.y<7.) {
            int byteIndex=row*48+col;
            int wordIndex=byteIndex/4;
            uint word=fx.hudText[wordIndex/4][wordIndex%4];
            uint c=(word>>uint((byteIndex%4)*8))&255u;
            if(letter(vec2(mod(q.x,6.),q.y),c))color=row==0?accent:ink;
        }
    }
    outColor=vec4(color,alpha);
}
void main() {
    vec2 p = panel;
    if(isHud!=0u){hudMain(p);return;}
    if(fx.inspector.x!=0u){inspectorMain(p);return;}
    vec3 color = mix(vec3(.035,.047,.071),vec3(.065,.078,.11),p.y/648.);
    if (p.x < 1 || p.x > 399 || p.y < 1 || p.y > 647) color = vec3(.22,.29,.38);
    if (p.y < 3) color = accent;
    bool explorer=fx.panel.w!=0u;
    if (textAt(p,vec2(24,24),explorer?65:0,explorer?1.5:2.)) color = ink;
    if (textAt(p,vec2(24,52),explorer?(fx.explorer[3].w!=0u?66:69):fx.panel.x==0u?1:fx.panel.z!=0u?29:30,1.)) color = fx.panel.x!=0u && fx.panel.z==0u?vec3(1,.73,.35):muted;
    if(fx.panel.x==0u) {
    if (box(p,vec4(24,80,18,18))) color = fx.effectEnabled != 0u ? accent : vec3(.20,.25,.32);
    if (fx.effectEnabled != 0u && box(p,vec4(29,85,8,8))) color = vec3(.04,.10,.12);
    if (textAt(p,vec2(54,83),2,2.)) color = ink;
    if (textAt(p,vec2(24,122),3,2.) || textAt(p,vec2(24,286),7,2.) || textAt(p,vec2(24,450),11,2.)) color = accent;
    float vals[6] = float[](fx.aoStrength,fx.aoRadius,fx.aoBias,fx.rainbowStrength,fx.hueFrequency,fx.opacity);
    float lows[6] = float[](0,1,0,0,.1,0), highs[6] = float[](3,32,.02,2,10,1);
    for (int i=0;i<6;i++) {
        float y = (i<3 ? 148. : 316.) + float(i%3)*44.;
        if (textAt(p,vec2(24,y),i<3 ? i+4 : i+5,2.)) color = ink;
        if (numberAt(p-vec2(0,y),vals[i],i==2 ? 4 : 2)) color = muted;
        float fraction = clamp((vals[i]-lows[i])/(highs[i]-lows[i]),0.,1.);
        if (box(p,vec4(24,y+25,352,4))) color = p.x < 24.+352.*fraction ? accent : vec3(.19,.24,.32);
        if (box(p,vec4(24.+352.*fraction-3.,y+21,6,12))) color = ink;
    }
    for (int i=0;i<4;i++) {
        vec2 origin = vec2(24+184*(i%2),478+32*(i/2));
        if (box(p,vec4(origin,168,26))) color = fx.debugMode==uint(i) ? vec3(.10,.27,.28) : vec3(.10,.13,.18);
        if (box(p,vec4(origin+vec2(8,9),7,7))) color = fx.debugMode==uint(i) ? accent : muted;
        if (textAt(p,origin+vec2(24,9),12+i,1.)) color = ink;
    }
    if (box(p,vec4(24,538,352,16))) color = fx.debugMode==4u ? vec3(.10,.27,.28) : vec3(.10,.13,.18);
    if (textAt(p,vec2(32,542),fx.debugMode==5u?26:25,1.)) color = fx.viewport.w > .5 ? ink : muted;
    } else {
        for(int i=0;i<10;i++) {
            vec4 row=fx.rows[i]; if(row.w<.5)continue;
            float y=78.+44.*float(i);
            if(textAt(p,vec2(24,y),int(row.z),1.5))color=ink;
            if(row.w==1.0) {
                if(box(p,vec4(354,y,18,18)))color=row.x>.5?accent:muted;
            } else if(row.w==3.0) {
                int label=-1;
                if(int(row.z)==15994)label=50+int(row.x);
                if(int(row.z)==27136)label=32+int(row.x);
                if(int(row.z)==3938)label=35+int(row.x);
                if(label>=0) {if(textAt(p,vec2(24,y+19),label,1.))color=accent;}
                else if(valueAt(p,vec2(268,y+18),row.x))color=accent;
            } else if(row.w==5.0) {
                if(textAt(p,vec2(24,y+20),35,1.))color=muted;
            } else if(row.w==8.0) {
                if(hexAt(p,vec2(24,y+20),int(row.x)))color=accent;
            } else if(row.w==10.0 || row.w==11.0 || row.w==12.0) {
                int label=row.w==10.0?70+clamp(int(row.x),0,2):row.w==11.0?75+clamp(int(row.x),0,4):74;
                if(textAt(p,vec2(24,y+20),label,1.))color=accent;
            } else {
                if(row.w==6.0) {
                    const vec2 sampleUV[3]=vec2[](vec2(.5,.5),vec2(.5,.1),vec2(.25,.75));
                    row.x=texture(diagnosticDepth,sampleUV[clamp(int(row.x),0,2)]).r;
                }
                if(valueAt(p,vec2(268,y+17),row.x))color=muted;
                if(row.w==2.0) {
                    if(box(p,vec4(24,y+30,240,4)))color=p.x<24.+240.*row.y?accent:vec3(.19,.24,.32);
                    if(box(p,vec4(24.+240.*row.y-3.,y+26,6,12)))color=ink;
                }
            }
        }
        if(box(p,vec4(24,538,352,16)))color=vec3(.10,.13,.18);
        if(textAt(p,vec2(32,542),28,1.))color=ink;
    }
    if(box(p,vec4(316,22,60,24)))color=vec3(.10,.27,.28);
    if(textAt(p,vec2(328,30),27,1.5))color=accent;
    if(letter((p-vec2(250,30))/1.5,48u+(fx.panel.x+1u)/10u) ||
       letter((p-vec2(260,30))/1.5,48u+(fx.panel.x+1u)%10u) ||
       letter((p-vec2(272,30))/1.5,47u) ||
       letter((p-vec2(284,30))/1.5,48u+fx.panel.y/10u) ||
       letter((p-vec2(294,30))/1.5,48u+fx.panel.y%10u))color=muted;
    for (int i=0;i<2;i++) {
        vec2 origin = vec2(24+184*i,556);
        if (box(p,vec4(origin,168,32))) color = i==0 ? vec3(.15,.36,.35) : vec3(.15,.19,.25);
        if (textAt(p,origin+vec2(explorer?12:i==0?60:42,9),explorer?67+i:16+i, explorer?1.:i==0?2.:1.)) color = ink;
    }
    vec4 focus;
    uint c = fx.menu.y;
    if(c==14u)focus=vec4(316,22,60,24);
    else if(fx.panel.x!=0u && c<10u)focus=vec4(24,78.+44.*float(c),352,38);
    else if(fx.panel.x!=0u && c==10u)focus=vec4(0);
    else if (c==13u) focus=vec4(24,538,352,16);
    else if (c==0u) focus=vec4(24,78,352,28);
    else if (c<=6u) focus=vec4(24,(c<=3u?148.:316.)+float((c-1u)%3u)*44.,352,38);
    else if (c<=10u) focus=vec4(24+184*((c-7u)%2u),478+32*((c-7u)/2u),168,26);
    else focus=vec4(24+184*(c-11u),556,168,32);
    if (box(p,focus+vec4(-3,-3,6,6)) && !box(p,focus+vec4(-2,-2,4,4))) color=accent;
    if (textAt(p,vec2(24,604),fx.menu.w==2u?24:fx.menu.w==1u?18:19,1.)) color = fx.menu.w==2u ? vec3(1.,.73,.35) : muted;
    int status = fx.menu.z==1u?20:fx.menu.z==2u?21:fx.menu.z==3u?22:23;
    if (textAt(p,vec2(24,624),explorer?73:status,1.)) color = fx.menu.z==2u ? vec3(1,.35,.35) : accent;
    // Software cursor remains visible if Wine uses an invisible X cursor.
    vec2 cursor = p-fx.pointer.xy;
    if (fx.pointer.z > 0. && cursor.y >= 0. && cursor.y < 16. && cursor.x >= 0. && cursor.x < cursor.y*.6+1.)
        color = cursor.x < 1. || cursor.x > cursor.y*.6-1. || cursor.y>14. ? vec3(.02) : ink;
    outColor = vec4(color,1.);
}
