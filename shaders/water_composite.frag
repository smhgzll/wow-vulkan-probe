#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform sampler2D camera;
layout(set=0,binding=1) uniform sampler2D waterMask;
layout(set=0,binding=2) uniform sampler2D sceneDepth;
layout(set=0,binding=3) uniform sampler2D sceneColor;
layout(set=0,binding=4) uniform sampler2D planarColor;
layout(set=0,binding=5) uniform sampler2D planarDepth;
layout(std140,set=0,binding=6) uniform Parameters {
    vec4 viewport;vec4 depthRange;uvec4 vertex;uvec4 indices;ivec4 draw;vec4 options;vec4 tone;vec4 foam;vec4 look;vec4 extra;vec4 fx;
    vec4 waveA[5];vec4 waveB[5];vec4 surface;   // the Gerstner waves (water_surface.glsl)
    vec4 modern;     // style (0 classic, 1 modern), GGX roughness, medium ripple scale, micro ripple scale
    vec4 sun;        // toward the sun (world), source (0 none, 1 game light, 2 ini fallback)
    vec4 sunColor;   // rgb sun colour, w specular strength
    vec4 body;       // absorption per yard rgb, subsurface strength
} params;
#include "water_surface.glsl"
layout(set=0,binding=7) uniform sampler2D skyMemory;
// Tileable water textures (tools/generate-water-textures.py). Layer 0: RG = ripple slope (+-1 = 3 sigma), A = height.
// Layer 1: R = lace foam (uniform values), G = second lace, B = bubble specks, A = wind streaks. Layer 2: R,G = caustic networks.
layout(set=0,binding=8) uniform sampler2DArray waterDetail;
// The water shader's own output (rgb, alpha) from the mask pass, before the game blended it over the scene.
layout(set=0,binding=9) uniform sampler2D waterColor;
// MODERN textures (tools/generate-modern-water-textures.py): layer 0 RG = ripple slope (+-1 = 3 sigma), layer 1 = foam (R main, G second,
// B bubble specks), layer 2 RGB = caustics (colour channels separated), A = luma. A flat 1x1 fallback when the file is missing.
layout(set=0,binding=10) uniform sampler2DArray modernDetail;
layout(location=0) out vec4 result;
// Layers (params.draw.w bit mask): 1 reflection, 2 refraction wobble, 4 depth tint and tone, 8 shore foam.
// params.tone: rgb = the water tone, w = how strongly it is applied (0 = natural).
// params.foam: x amount, y patch size, z foam clock (seconds, already scaled by the speed setting), w softness.
// params.look: x come-and-go strength, y shore reach, z texture detail, w quality (0..2). params.extra: x opacity of the game's own water texture, y the game's water blend (0 unknown, 1 alpha, 2 premultiplied).
// params.fx: x shallow clarity, y seabed caustics, z sun glitter, w open-sea whitecaps.
// params.options.z: debug view (water_reflections.hpp waterDebugViewNames): 1 mask, 2 normal, 3 water depth, 4 reflection, 5 base surface,
// 6 displacement, 7 large waves.
vec4 column(int x) { return texelFetch(camera,ivec2(x,0),0); }
// Window position of a view-space point (+Z forward), as the Vulkan viewport transform maps it. A negative
// viewport height (WoW: 1080.5, -1080) simply flips Y here.
vec2 windowOf(vec3 p,vec4 projection,vec4 viewport) {
    vec2 ndc=p.xy*projection.zw/p.z;
    return viewport.xy+(ndc*.5+.5)*viewport.zw;
}
// View depth of a stored depth value: undo the viewport's min/max depth, then the finite +Z projection.
float linearDepth(float raw,vec4 range,vec4 coefficients) {
    float d=(raw-range.x)/(range.y-range.x);
    return coefficients.w/(d-coefficients.z);
}
// World position (WoW: z up) from a view-space point: view = R*world + t with R orthonormal, so world = R^T*(view - t).
// The columns of the view matrix are texels 14..17 of the camera texture.
vec3 worldOf(vec3 v,vec3 c5,vec3 c6,vec3 c7,vec3 c8) {
    vec3 t=v-c8;
    return vec3(dot(t,c5),dot(t,c6),dot(t,c7));
}
// Medium detail: three short travelling waves (.7, 1.3, 2.6 yards) as slopes only (too short to displace anything the eye could see).
// Direction, k = 2*pi/wavelength, amplitude*k and phase speed, folded at build time. They fade with distance: they would only alias and
// shimmer. The long waves are the Gerstner surface (water_surface.glsl).
const vec2 waveDir[3]=vec2[3](vec2(.95534,.29552),vec2(-.32329,.94630),vec2(-.94222,.33499));
const float waveK[3]=float[3](8.97598,4.83322,2.41661);
const float waveSlopeAmp[3]=float[3](.089760,.067665,.048332);
const float waveSpeed[3]=float[3](1.7,1.3,.9);
vec2 mediumSlopes(vec2 p,float time,float distanceToCamera) {
    vec2 slope=vec2(0.);
    for(int i=0;i<3;++i) {
        float k=waveK[i],c=cos(k*dot(waveDir[i],p)+waveSpeed[i]*time);
        slope+=waveDir[i]*(waveSlopeAmp[i]*c/(1.+distanceToCamera*k*.04));
    }
    return slope;
}
// Slope from the ripple texture: two scrolling copies at different sizes (17 and 6.3 yard tiles), sampled at the mip level that matches one
// pixel's footprint (footprint in yards), so distant water flattens instead of shimmering.
vec2 detailSlope(vec2 p,float time,float footprint) {
    float lodA=max(log2(footprint*256./17.),0.),lodB=max(log2(footprint*256./6.3),0.);
    vec2 a=textureLod(waterDetail,vec3(p/17.+vec2(.62,.24)*time*.020,0.),lodA).rg*2.-1.;
    vec2 b=textureLod(waterDetail,vec3(p/6.3+vec2(-.20,.55)*time*.034+.37,0.),lodB).rg*2.-1.;
    return a*.70+b*.50;
}
// Finer ripple slopes for sun glitter (1.7 and .83 yard tiles), mip-filtered like everything else: far water stops sparkling by itself.
vec2 glintSlope(vec2 p,float time,float footprint) {
    float lod=max(log2(footprint*256./1.7),0.);
    vec2 a=textureLod(waterDetail,vec3(p/1.7+vec2(.05,.03)*time,0.),lod).rg*2.-1.;
    vec2 b=textureLod(waterDetail,vec3(p/.83+vec2(-.04,.06)*time+.21,0.),lod+1.03).rg*2.-1.;
    return a+.6*b;
}
// Caustics: two bright networks that scroll apart, multiplied, so the lines shimmer and cross in bright nodes.
float caustic(vec2 p,float time,float footprint) {
    float lod=max(log2(footprint*256./2.6),0.)+.9;                                     // a little blur: focused light is soft-edged
    float a=textureLod(waterDetail,vec3(p/3.2+vec2(.037,.021)*time,2.),lod).r;
    float b=textureLod(waterDetail,vec3(p/2.1+vec2(-.029,.041)*time+.5,2.),lod+.6).g;
    return sqrt(a*b);
}
// Height of the Gerstner surface at rest position p, -1..1 of the waves' total amplitude (0 without waves). delay (seconds) gives what
// stood there a moment ago.
float crestHeight(vec2 p,float footprint,float delay) {
    return params.surface.y>1e-5?waterSurfaceAtTime(p,0.,footprint,delay).displacement.z/params.surface.y:0.;
}
float hash(vec2 p) { p=fract(p*vec2(123.34,456.21));p+=dot(p,p+45.32);return fract(p.x*p.y); }
// The lattice repeats every 251 cells; the wrap is applied to the cell coordinates (both corners), so the noise stays continuous across it.
float valueNoise(vec2 p) {
    vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);
    vec2 i0=mod(i,251.),i1=mod(i+vec2(1.),251.);
    return mix(mix(hash(i0),hash(vec2(i1.x,i0.y)),f.x),mix(hash(vec2(i0.x,i1.y)),hash(i1),f.x),f.y);
}
// Five-tap cross blur of the scene image: reflections of far things are softer, like rough water; it also hides the single-pixel
// holes a fixed step march leaves.
vec3 softScene(ivec2 texel,float radius,ivec2 limit) {
    int r=int(max(1.,radius));
    vec3 c=texelFetch(sceneColor,texel,0).rgb*2.;
    c+=texelFetch(sceneColor,clamp(texel+ivec2(r,0),ivec2(0),limit),0).rgb;
    c+=texelFetch(sceneColor,clamp(texel-ivec2(r,0),ivec2(0),limit),0).rgb;
    c+=texelFetch(sceneColor,clamp(texel+ivec2(0,r),ivec2(0),limit),0).rgb;
    c+=texelFetch(sceneColor,clamp(texel-ivec2(0,r),ivec2(0),limit),0).rgb;
    return c/6.;
}
// The sky the reflected direction (view space) points at, from the sky memory (world azimuth / elevation). A NaN texel can only come from a
// never-initialised map; it falls back to the default gradient.
vec3 skyColor(vec3 direction,vec3 c5,vec3 c6,vec3 c7) {
    vec3 world=vec3(dot(direction,c5),dot(direction,c6),dot(direction,c7));
    vec2 uv=vec2(atan(world.y,world.x)/6.2831853+.5,asin(clamp(world.z,0.,1.))/1.5707963);
    vec3 color=texture(skyMemory,uv).rgb;
    if(any(isnan(color)) || any(isinf(color)))color=mix(vec3(.80,.66,.52),vec3(.30,.45,.75),pow(uv.y,.5));
    return max(color,vec3(0.));
}
// ---- MODERN helpers ----
// Ripple texture slope (layer 0 RG: +-1 = 3 sigma, so one sample has an rms of about 1/3).
vec2 rippleSlope(vec2 uv,float lod) { return textureLod(modernDetail,vec3(uv,0.),lod).rg*2.-1.; }
float modernTexels() { return float(textureSize(modernDetail,0).x); }
// One octave of medium ripples: Crest's pair of samples scrolling in opposing directions; the second rotated 67 degrees (and its slope rotated
// back) because the texture's ripples are wind-aligned and two copies in one orientation read as parallel bands far away. Speeds follow
// deep-water dispersion (phase speed ~ sqrt(size)).
vec2 ripplePair(vec2 q,float size,float time,float footprint) {
    const vec2 d0=vec2(.94,.34),d1=vec2(-.85,-.53);
    const mat2 turn=mat2(.3907,.9205,-.9205,.3907);
    // one mip sharper than the isotropic footprint asks: at grazing angles the footprint is the long axis, which blurs the short one
    // (hardware anisotropic filtering keeps it; this is its cheap stand-in; TUNING)
    float lod=max(log2(max(footprint*modernTexels()/size,1e-4))-1.,0.),speed=.315*sqrt(size/7.)/size;
    return rippleSlope(q/size+d0*time*speed,lod)+turn*rippleSlope(transpose(turn)*q/(size*1.13)+d1*time*speed*1.15+.37,lod+.18);
}
// Medium ripples (world slope, rms ~.235 before the caller's scale), world-anchored and detiled by a slow world-space offset (LesusX). The tile
// size grows by octaves with the pixel footprint (7, 14, 28 ... yards), so the ripples keep their size on screen far away instead of being
// averaged flat - Crest scales its normal maps with the LOD the same way; a flat far sea read matte next to the classic look (35169-1).
vec2 mediumRipples(vec2 p,float time,float footprint) {
    vec2 detile=vec2(valueNoise(p*.021+vec2(3.1,7.7)),valueNoise(p*.021+vec2(11.3,1.9)))-.5;
    vec2 q=p+detile*6.;
    // octave: a tile spans ~45-90 pixels on screen, never less than 7 yards (TUNING). The texture's features (10-30 texels of 1024) then
    // land at a few pixels - the fine, living surface. Tiles hundreds of pixels wide read as large blotches (48186-1).
    float octave=clamp(log2(max(footprint*256./7.,1e-4))-2.5,0.,8.),o0=floor(octave),blend=octave-o0,size=7.*exp2(o0);
    vec2 a=ripplePair(q,size,time,footprint);
    vec2 b=blend>.001?ripplePair(q+vec2(17.3,-9.1),size*2.,time,footprint):a;
    return mix(a,b,blend)*.5;
}
// Micro ripples (rms ~.235 before the caller's scale), 1.6- and .9-yard tiles in other directions and speeds; lod = the mip level used (what the
// filter averaged away turns into roughness).
vec2 microRipples(vec2 p,float time,float footprint,out float lod) {
    lod=max(log2(max(footprint*modernTexels()/1.6,1e-4)),0.);
    const mat2 turn=mat2(.5,-.866,.866,.5);   // -60 degrees: crossed with the first
    return (rippleSlope(p/1.6+vec2(.24,-.97)*time*.11,lod)+turn*rippleSlope(transpose(turn)*p/.9+vec2(-.6,.8)*time*.15+.21,lod+.83))*.5;
}
// MODERN caustics (Crest's method): the bed point projected along the sun to the surface (with a factor 4 against the stretching a full
// projection gives on slopes), two samples of the chromatic caustic texture scrolling at different scales, bent by the ripple normals, minus
// the texture's own mean (its 1x1 mip), so the bed keeps its average brightness and gains bright, colour-fringed lines. Sharpest at a focal depth
// of ~2 yards (mip bias with the distance from it: TUNING). Tile 4.5 yards (TUNING).
vec3 modernCaustics(vec2 bedXY,float depth,float time,float footprint,vec3 sunWorld,bool sunOk) {
    vec2 p=bedXY+(sunOk?sunWorld.xy*depth/(4.*max(sunWorld.z,.15)):vec2(0.));
    vec2 bend=rippleSlope(p/5.+vec2(.013,-.021)*time,0.)*.09;
    float lod=max(log2(max(footprint*modernTexels()/4.5,1e-4)),0.)+abs(depth-2.)*.5;
    vec3 a=textureLod(modernDetail,vec3(p/4.5+vec2(.0098,-.0376)*time+bend*1.3,2.),lod).rgb;
    vec3 b=textureLod(modernDetail,vec3(p/3.28+vec2(.0551,.026)*time+bend*1.77+.5,2.),lod+.3).rgb;
    return .5*a+.5*b-textureLod(modernDetail,vec3(.5,.5,2.),20.).rgb;
}
// Did the game draw its water at this pixel? Where it did, the scene holds a W + (1 - a) S with the bed S a colour, inside [0, 1]. Outside
// that the pixel is something else, and undoing a blend that did not happen subtracts water that is not there, amplified by 1 / (1 - a) ~ 5:
// the terrain just in front of the water plane by less than the depth slack (the game's depth test failed, ours passed) turned into a black
// or saturated red line along every shore, and a blended effect drawn over the water later in the pass (a fishing pool's swirl) into a
// purple ring (both in game, session 39664; the fog capture showed them before the fog). Such pixels are left as the game drew them. Margin
// .06: 8-bit rounding x 1 / (1 - a) is ~.01 at the measured sea alpha (.77-.82).
bool gameWaterAt(ivec2 at) {
    float blendMode=params.extra.y;
    vec4 own=texelFetch(waterColor,at,0);
    float a=own.a;
    if(blendMode<.5 || a<.02 || a>.995)return true;   // no blend to undo there: nothing to check
    vec3 s=(texelFetch(sceneColor,at,0).rgb-(blendMode<1.5?own.rgb*a:own.rgb))/(1.-a);
    return all(greaterThan(s,vec3(-.06))) && all(lessThan(s,vec3(1.06)));
}
// View-space point of the scene depth at a pixel (false where nothing was drawn).
bool sceneViewAt(ivec2 p,vec2 extent,vec4 range,vec4 projection,vec4 viewport,vec4 coefficients,out vec3 v) {
    p=clamp(p,ivec2(0),ivec2(extent)-1);
    float raw=texelFetch(sceneDepth,p,0).r;
    if(raw>=range.y-1e-6)return false;
    vec2 ndc=(vec2(p)+.5-viewport.xy)/viewport.zw*2.-1.;
    v=vec3(ndc/projection.zw,1.)*linearDepth(raw,range,coefficients);
    return true;
}
// How horizontal the bed under a pixel is, 0 (a rock face) .. 1 (flat): the normal from the scene depth 3 pixels around (WoW's terrain is
// faceted; one pixel turned every triangle on or off), each axis from the neighbour with the smaller depth step (no normal across an edge),
// against world up (view space). Faded from 70 to 37 degrees of slope (HEURISTIC).
float bedFlatness(ivec2 p,vec2 extent,vec4 range,vec4 projection,vec4 viewport,vec4 coefficients,vec3 up) {
    vec3 v,a,b,c,d;
    if(!sceneViewAt(p,extent,range,projection,viewport,coefficients,v))return 1.;
    bool ha=sceneViewAt(p+ivec2(3,0),extent,range,projection,viewport,coefficients,a),hb=sceneViewAt(p-ivec2(3,0),extent,range,projection,viewport,coefficients,b);
    bool hc=sceneViewAt(p+ivec2(0,3),extent,range,projection,viewport,coefficients,c),hd=sceneViewAt(p-ivec2(0,3),extent,range,projection,viewport,coefficients,d);
    vec3 dx=ha && (!hb || abs(a.z-v.z)<abs(v.z-b.z))?a-v:hb?v-b:vec3(1,0,0);
    vec3 dy=hc && (!hd || abs(c.z-v.z)<abs(v.z-d.z))?c-v:hd?v-d:vec3(0,1,0);
    vec3 n=cross(dx,dy);
    float len=length(n);
    if(len<1e-12)return 1.;
    return smoothstep(.35,.8,abs(dot(n/len,up)));
}
// Cubic B-spline read of one mip level from four bilinear taps (Sigg & Hadwiger 2005): the level's texels without bilinear's creases, which
// read as blocky blotches wherever a coarse level is magnified over a large part of the picture (the water tint, 32267-2 seen from above).
vec4 textureBSpline(sampler2D s,vec2 uv,int level) {
    vec2 size=vec2(textureSize(s,level)),p=uv*size-.5,i=floor(p),f=p-i,f2=f*f,f3=f2*f;
    vec2 w0=(1.-3.*f+3.*f2-f3)/6.,w1=(4.-6.*f2+3.*f3)/6.,w2=(1.+3.*f+3.*f2-3.*f3)/6.,w3=f3/6.;
    vec2 g0=w0+w1,g1=w2+w3,t0=(i-.5+w1/g0)/size,t1=(i+1.5+w3/g1)/size;
    float lod=float(level);
    return g0.y*(g0.x*textureLod(s,vec2(t0.x,t0.y),lod)+g1.x*textureLod(s,vec2(t1.x,t0.y),lod))+
           g1.y*(g0.x*textureLod(s,vec2(t0.x,t1.y),lod)+g1.x*textureLod(s,vec2(t1.x,t1.y),lod));
}
// The screen and the sky memory are LDR: the bright sky near a low sun is clipped at 1, and a Fresnel-weighted reflection of it came out dull.
// Expand the bright end back toward HDR (inverse Reinhard on luma, x1.8 at white; HEURISTIC).
vec3 expandBright(vec3 c) {
    float l=dot(c,vec3(.299,.587,.114));
    return c*((1./(1.-min(l,.98)*.45))*(l>1e-4?1.:0.)+(l>1e-4?0.:1.));
}
// The sky memory seen through a rough surface: the reflected direction and four around it, spread with the GGX alpha.
vec3 skyRough(vec3 direction,float alpha,vec3 c5,vec3 c6,vec3 c7) {
    vec3 c=skyColor(direction,c5,c6,c7);
    if(alpha<.08)return c;
    vec3 t1=normalize(cross(direction,abs(direction.y)<.9?vec3(0.,1.,0.):vec3(1.,0.,0.))),t2=cross(direction,t1);
    float k=alpha*.6;
    c+=skyColor(normalize(direction+t1*k),c5,c6,c7)+skyColor(normalize(direction-t1*k),c5,c6,c7)+
       skyColor(normalize(direction+t2*k),c5,c6,c7)+skyColor(normalize(direction-t2*k),c5,c6,c7);
    return c/5.;
}
// MODERN whitecaps (Crest / Tessendorf): foam where the waves compress the surface - the Jacobian of the horizontal displacement falls below
// 1 on the crests - and for a moment after (the same spot .7 and 1.4 s earlier, fading). The compression is taken relative to the most the
// configured waves can reach (sum Q k A: J >= 1 - sum Q k A), so only the strongest few percent of the crests foam. How much is gated by the
// waves' steepness sum k A: real waves break only when steep (the Stokes limit is k A ~ .44 for one wave), so WoW's default calm swell
// (sum k A ~ .09 at wave height 1) gets none, wave height 2 (~.18) a trace, 4 (~.37) the full amount. Thresholds TUNING. The old height
// threshold foamed every high spot: white smears far out (13655-1), isolated white blobs in the calm harbour (13655-5).
float jacobianCrest(vec2 x0,float footprint) {
    float qka=params.surface.z;
    if(qka<1e-4)return 0.;
    float crest=0.;
    for(int j=0;j<3;++j) {
        float compression=(1.-waterSurfaceAtTime(x0,0.,footprint,float(j)*.7).jacobian)/qka;
        crest=max(crest,smoothstep(.62,.97,compression)*(j==0?1.:j==1?.55:.25));
    }
    return crest*smoothstep(.12,.36,params.surface.w);
}
// Foam over `color` (method: see the call site). viewDepth = the base surface's view depth (distances fade the lace), sky = the reflected sky.
// crestIn >= 0: the whitecap amount computed by the caller (MODERN: jacobianCrest x whitecaps); < 0: from the crest height (CLASSIC).
vec3 applyFoam(vec3 color,vec2 worldXY,float depthBelow,float footprint,float viewDepth,vec3 sky,sampler2DArray foamTexture,float crestIn) {
    float texels=float(textureSize(foamTexture,0).x);
    float foamAmount=params.foam.x,whitecaps=params.fx.w;
    if(foamAmount>0. || whitecaps>0.) {
        float size=max(params.foam.y,.05),t=params.foam.z,softness=params.foam.w,pulse=params.look.x,reach=params.look.y;
        float envelope=smoothstep(2.6*reach,0.,depthBelow);
        float shorePresence=foamAmount>0.?envelope*(1.-smoothstep(30.,120.,viewDepth)):0.;      // scene depth is too coarse far away
        // Open-sea whitecaps: foam where the waves peak, and for a moment after a crest has passed (the same spot a little earlier in time).
        float crest=0.;
        if(whitecaps>0. && viewDepth<220.) {
            if(crestIn>=0.)crest=crestIn;
            else {
                float threshold=.75-.40*whitecaps;
                for(int j=0;j<3;++j)crest=max(crest,smoothstep(threshold,threshold+.22,crestHeight(worldXY,footprint,float(j)*.7))*(j==0?1.:j==1?.6:.3));
            }
            crest*=1.-smoothstep(70.,220.,viewDepth);
        }
        if(shorePresence>.001 || crest>.001) {
            vec2 p=worldXY-vec2(.28,.11)*t;                                                  // the whole pattern drifts with the water
            vec2 patchP=p/(9.*size);
            vec2 warp=vec2(valueNoise(patchP*.8+vec2(t*.016,0.)),valueNoise(patchP*.8+vec2(5.2,t*.013)))-.5;   // slow swirl
            vec2 q=patchP+warp*1.6;
            float cover=valueNoise(q)*.62+valueNoise(q*2.13+7.7)*.38;                        // cloud field, mean .5
            float clump=smoothstep(.28,.62,cover);
            float phase=valueNoise(patchP*1.3+31.7)*6.2831853;
            float life=.5+.5*sin(t*.55+phase)*(.5+.5*sin(t*.23+phase*1.7));                  // irregular 0..1 rhythm per patch
            float lodA=max(log2(footprint*texels/(3.6*size)),0.),lodB=max(log2(footprint*texels/(1.9*size)),0.);
            vec4 lace2=textureLod(foamTexture,vec3(p/(1.9*size)+.31+vec2(.011,-.008)*t/(1.9*size),1.),lodB);
            float period=1.3*size;
            // The front's edge is torn at lace scale (lace2.g) so it is never a smooth stroke; foam trails out to sea from it.
            float frac=fract((depthBelow+warp.x*1.6*size)/period+t*.08+(lace2.g-.5)*.10);   // waves run toward the shore (depth falls with time)
            float front=exp(-frac*4.0);
            float nearOnly=1.-smoothstep(12.,45.,viewDepth);
            float waterline=smoothstep(.65*size,0.,depthBelow)*nearOnly;                     // the wet edge carries foam, close up
            float rhythm=mix(1.,.35+1.25*life,pulse);
            float amount=envelope*(.16+.84*front)*(.30+.85*clump)+waterline*.80*(.40+.60*clump);
            amount*=rhythm;
            amount*=1.-.55*smoothstep(15.,70.,viewDepth);                                        // mip-averaged far lace would turn full amounts into a white band
            amount=smoothstep(.06,.95,clamp(amount*min(foamAmount,1.6),0.,1.))*.90;         // sparse specks vanish, solid foam always keeps holes
            float crestAmount=smoothstep(.06,.95,clamp(crest*(.35+.85*clump)*rhythm,0.,1.))*.62;   // never solid: whitecaps keep their holes
            float lace1=textureLod(foamTexture,vec3(p/(3.6*size),1.),lodA).r;
            float value=lace1+(lace2.g-.5)*.28+lace2.b*.06;
            float edge=mix(.015,.20,softness)+.30*clamp(lodA/4.,0.,1.);                      // mip-averaged (far) lace has no structure: soften
            float foam=max(smoothstep(1.-amount-edge,1.-amount+edge,value)*shorePresence,
                           crest>.001?smoothstep(1.-crestAmount-edge,1.-crestAmount+edge,value):0.);
            foam*=1.-.45*smoothstep(20.,90.,viewDepth);
            vec3 foamColor=vec3(.95,.98,.99)*(.84+.16*clamp(dot(sky,vec3(.299,.587,.114))*1.3,0.,1.))*(.86+.14*lace1);
            color=mix(color,foamColor,clamp(foam,0.,1.)*.92);
        }
    }
    return color;
}
// Screen-space reflection march in equal pixel steps (perspective-correct through 1/z), so it reaches far surfaces cheaply. True on a hit:
// hit = its window position, travel = pixels from the start.
bool ssrMarch(vec3 origin,vec3 direction,vec2 extent,vec4 range,vec4 projection,vec4 viewport,vec4 coefficients,out vec2 hit,out float travel) {
    hit=vec2(0.);travel=0.;
    float tmax=1500.;
    vec3 end=origin+direction*tmax;
    if(end.z<projection.x+.01 && direction.z<0.)end=origin+direction*((projection.x+.01-origin.z)/direction.z);
    if(end.z>projection.y && direction.z>0.)end=origin+direction*((projection.y-origin.z)/direction.z);
    if(!(end.z>projection.x))return false;
    vec2 w0=windowOf(origin,projection,viewport),w1=windowOf(end,projection,viewport);
    float k0=1./origin.z,k1=1./end.z;
    vec2 delta=w1-w0;float pixels=max(abs(delta.x),abs(delta.y));
    // Quality: low / medium / high. Steps are dense near the water (where reflected shores and objects need accuracy) and grow sparser
    // toward the far end of the ray (a ray of 450 px takes about 55 steps instead of 112 fixed ones): position a = (i/N)^1.5.
    float quality=params.look.w,stepPixels=quality<.5?6.:quality<1.5?4.:3.,maxSteps=quality<.5?32.:quality<1.5?64.:128.;
    float steps=clamp(ceil(1.5*pow(pixels/stepPixels,.75)),1.,maxSteps);
    float previous=-1.,previousZ=origin.z;
    for(int i=1;i<=128;++i) {
        if(float(i)>steps)break;
        float u=float(i)/steps,a=u*sqrt(u);   // (i/N)^1.5 without pow()
        vec2 w=mix(w0,w1,a);
        if(any(lessThan(w,vec2(0.))) || any(greaterThanEqual(w,extent)))break;      // left the picture
        float rayZ=1./mix(k0,k1,a);
        float raw=texelFetch(sceneDepth,ivec2(w),0).r;
        bool background=raw>=range.y-1e-6;
        // Thickness follows the depth the ray covers per step: at grazing angles a fixed value lets the ray hop through a
        // surface between two samples, which is what left a dotted, sandy reflection.
        float gap=rayZ-linearDepth(raw,range,coefficients),thick=.3+abs(rayZ-previousZ)*1.6+rayZ*rayZ*1.5e-6;
        previousZ=rayZ;
        if(!background && previous<0. && gap>=0. && gap<thick && length(w-w0)>4.){hit=w;travel=length(w-w0);return true;}
        previous=background?-1.:gap;
    }
    return false;
}
void main() {
    result=vec4(0);
    vec4 coefficients=column(13);if(coefficients.x<.5)return;
    vec2 extent=vec2(textureSize(waterMask,0)),uv=gl_FragCoord.xy/extent;
    ivec2 here=ivec2(gl_FragCoord.xy);
    vec4 water=texelFetch(waterMask,here,0);if(water.a<.0001)return;
    vec4 range=column(12),projection=column(10),viewport=column(11);
    float waterRaw=mix(range.x,range.y,coefficients.z+coefficients.w/water.z);
    float rawHere=texelFetch(sceneDepth,here,0).r;
    // The game's own depth test (LESS_OR_EQUAL against the same D24 depth): its water is exactly where scene - water >= 0. MEASURED on 48451-2
    // (a cliff at the shore): every pixel with a difference of -3e-7 or below is the cliff without water. The old slack of 2e-5 took a band
    // of rock along every shore for water - undoing a blend that never happened made it a black / orange line (gameWaterAt alone cannot tell
    // a bright rock from a plausible bed). 2e-7 = ~3 D24 steps; the mask's depth reconstruction errs by far less.
    if(rawHere+2e-7<waterRaw)return;
    vec4 plane=column(9),reference=column(18);   // reference: (plane valid, plane height, camera world z, 0)
    vec3 c5=column(14).xyz,c6=column(15).xyz,c7=column(16).xyz,c8=column(17).xyz;
    // planeView.(p,1) is the surface's world height above the validated plane. The depth-derived position is only as exact as the
    // 24-bit fragment depth (about z*z*3e-7 view units), so the tolerance grows with distance.
    bool onPlane=reference.x>.5 && abs(dot(plane.xyz,water.xyz)+plane.w)<=.05+6e-7*water.z*water.z;
    // Another water level (WoW: Shadowglen's two ponds at z 1306.97 and 1308.36). Taken only where that surface is horizontal:
    // neighbouring water pixels at the same world height (a sloped river or a step between levels is left as the game drew it).
    float surfaceHeight=onPlane?reference.y:dot(water.xyz-c8,c7);
    if(!onPlane) {
        if(surfaceHeight>=reference.z-.001)return;   // seen from below (or at eye level): left as the game drew it
        const ivec2 around[4]=ivec2[4](ivec2(3,0),ivec2(-3,0),ivec2(0,3),ivec2(0,-3));
        ivec2 last=textureSize(waterMask,0)-1;
        // A slope is only seen along an axis with water on at least one side: a rim pixel whose only water neighbours lie
        // along the surface's level direction would pass any slope (test: tilted sea). Both screen axes must be testable.
        // Cost on real captures: at most 0.08% of the water pixels (isolated edge pixels).
        bvec2 tested=bvec2(false);
        for(int i=0;i<4;++i) {
            vec4 n=texelFetch(waterMask,clamp(here+around[i],ivec2(0),last),0);
            // Slope, not a fixed height step: 3 px near the camera is only ~.1 yd, where a .02 height tolerance let a 17%
            // slope pass as flat. Allowed: depth precision + 3% of the horizontal distance between the two points. On real
            // captures (sea, Shadowglen ponds) this accepts all the flat water the old test did (+0.16% at pond edges).
            vec3 step=n.xyz-water.xyz;
            float horizontal=length(step-c7*dot(step,c7));
            if(n.a<=.0001)continue;
            if(i<2)tested.x=true;else tested.y=true;
            if(abs(dot(n.xyz-c8,c7)-surfaceHeight)>.004+2e-6*n.z*n.z+.03*horizontal)return;
        }
        if(!all(tested))return;
    }
    vec3 flatNormal=normalize(plane.xyz);   // world up in view space: the same for every horizontal level
    if(dot(flatNormal,-water.xyz)<0)flatNormal=-flatNormal;
    float time=params.options.w;
    // Size of one pixel on the water (yards): picks the texture mip level and the waves' level of detail. At grazing angles it stretches
    // along the view direction. From the base plane, so it is the same whatever the waves do.
    float cosView=max(dot(flatNormal,-normalize(water.xyz)),.05);
    float footprint=min(water.z*2./(projection.w*extent.y)/cosView,64.);
    // ---- the moving surface (water_surface.glsl). The base plane is the level the game draws: flat, it defines the coverage (the mask) and
    // stays what the fog stops at. The pixel's view ray is intersected with the Gerstner-displaced surface over it, and everything below uses
    // that one surface: its point on the ray (reflection origin, water depth), its normal (reflection, refraction, glints) and the rest
    // position x0 of the water there (textures, foam and caustics ride the waves with it). ----
    vec3 cameraWorld=worldOf(vec3(0.),c5,c6,c7,c8),flatWorld=worldOf(water.xyz,c5,c6,c7,c8),hitWorld;
    WaterSurface wave;
    if(!waterSurfaceHit(cameraWorld,flatWorld-cameraWorld,surfaceHeight,footprint,wave,hitWorld)){wave=waterSurfaceAt(flatWorld.xy,surfaceHeight,1e9);hitWorld=flatWorld;}
    // The same ray as water.xyz, scaled by the ratio of heights below the camera (no large world coordinates in the difference).
    vec3 surfaceView=water.xyz*((hitWorld.z-cameraWorld.z)/min(surfaceHeight-cameraWorld.z,-1e-4));
    vec2 worldXY=wave.parameter;
    vec2 largeSlope=-wave.normal.xy/max(wave.normal.z,.2);   // the Gerstner surface's slope (world)
    // Normal: the large waves' slope plus the medium ripples and the texture detail (world normal (-sx,-sy,1), taken into view space).
    vec2 slope=largeSlope+mediumSlopes(worldXY,time,water.z);
    float detail=params.look.z;
    if(detail>0.)slope+=detailSlope(worldXY,time,footprint)*(detail*.48);
    vec3 normal=normalize(c5*-slope.x+c6*-slope.y+c7);
    if(dot(normal,-water.xyz)<0)normal=-normal;
    int debug=int(params.options.z),layers=params.draw.w;
    if(debug==1){result=vec4(.05,.8,1,1);return;}
    if(debug==2){result=vec4(normal*.5+.5,1);return;}
    if(debug==3){result=vec4(vec3(clamp(water.z/40,0,1)),1);return;}
    if(debug==5) {   // BASE SURFACE: the flat level, a world-anchored 1-yard grid (lines one pixel wide); green = the validated plane, blue = another level
        vec2 g=abs(fract(flatWorld.xy+.5)-.5)/max(footprint,1e-3);
        result=vec4(mix(onPlane?vec3(.10,.45,.20):vec3(.10,.25,.50),vec3(1.),(1.-clamp(min(g.x,g.y),0.,1.))*.8),1);return;
    }
    if(debug==6){result=vec4(clamp(vec3(.5)+wave.displacement.zxy*2.,0.,1.),1);return;}   // DISPLACEMENT: r = height, g/b = horizontal orbit x/y (+-.25 yd full scale)
    if(debug==7){result=vec4(clamp(.5+largeSlope*5.,0.,1.),clamp(wave.jacobian,0.,1.),1);return;}   // LARGE WAVES: rg = Gerstner slope x5, b = Jacobian
    // ======== MODERN style (docs/water-references.md): a layered surface under Fresnel and roughness. The debug views 8.. always come from here. ========
    if(params.modern.x>.5 || debug>=8) {
        if(!gameWaterAt(here))return;   // not the game's water here (see gameWaterAt): left as drawn
        vec3 V=-normalize(surfaceView),up=flatNormal;
        // ---- multi-scale normal (world slopes): large = the Gerstner surface, medium and micro ripples on top. The micro slope variance the mip
        // filter averages away with distance becomes roughness (Toksvig-style; the averaged fraction 1 - 2^-lod is HEURISTIC). ----
        vec2 medium=mediumRipples(worldXY,time,footprint)*(.30*params.modern.z);
        float microLod;
        vec2 micro=microRipples(worldXY,time,footprint,microLod)*(.34*params.modern.w);
        vec2 slopeF=largeSlope+medium+micro;
        vec3 nF=normalize(c5*-slopeF.x+c6*-slopeF.y+c7);
        float nv=dot(nF,V);if(nv<.02)nF=normalize(nF+V*(.02-nv));   // a facet turned away at a grazing view: bent back toward the eye
        float microVar=pow(.235*.34*params.modern.w,2.);
        // Only half the lost variance widens the lobe (HEURISTIC): the full Toksvig width turned the whole sun path into one flat patch (18039-1).
        float alpha=clamp(sqrt(params.modern.y*params.modern.y+microVar*(1.-exp2(-microLod))),.02,1.);
        float NV=clamp(dot(nF,V),0.,1.);
        const float R0=.02;                                                   // ((1 - 1.333) / (1 + 1.333))^2: MEASURED
        float fresnel=R0+(max(1.-alpha,R0)-R0)*pow(1.-NV,5.);                 // Schlick with the roughness-aware ceiling (Lagarde)
        // ---- the water depth under the pixel: the view ray's path inside the water (surface to the seabed the scene depth holds; nothing drawn
        // behind the water = deep) and the vertical depth for the foam ----
        float pathLength=1e3,depthBelow=40.;
        if(rawHere<range.y-1e-6) {
            vec2 ndc=(gl_FragCoord.xy-viewport.xy)/viewport.zw*2.-1.;
            vec3 seabed=vec3(ndc/projection.zw,1.)*linearDepth(rawHere,range,coefficients);
            pathLength=max(length(seabed)-length(surfaceView),0.);
            depthBelow=max(0.,hitWorld.z-dot(seabed-c8,c7));
        }
        // ---- refraction (layer 2): the bed seen through the tilted surface, shifted with the normal's tilt, more through thicker water and less
        // far away; a sample that lands on something in front of the water (or off the water) is not used (Crest / Mistral) ----
        ivec2 under=here;float refractPx=0.;
        if((layers&2)!=0 || debug==19) {
            vec2 offset=(nF-up).xy*extent.y*.035*min(1.,pathLength*.5)/(1.+surfaceView.z*.015);
            ivec2 q=ivec2(clamp(gl_FragCoord.xy+offset,vec2(0.),extent-1.));
            vec4 qw=texelFetch(waterMask,q,0);
            if(qw.a>0. && texelFetch(sceneDepth,q,0).r+2e-7>=mix(range.x,range.y,coefficients.z+coefficients.w/qw.z) && gameWaterAt(q)){under=q;refractPx=length(offset);}
        }
        // ---- what lies under the surface: the scene holds the game's blend of its water W over the bed S; with the blend known S is recovered
        // (scene - a W) / (1 - a), at the refracted pixel with that pixel's own W and a ----
        float blendMode=params.extra.y;
        vec4 own=texelFetch(waterColor,under,0);
        float a=clamp(own.a,0.,1.);
        vec3 sceneUnder=texelFetch(sceneColor,under,0).rgb,S=sceneUnder;
        vec3 W=blendMode>1.5?own.rgb/max(a,1e-3):own.rgb;                     // the game's own water colour: its zone tint and texture
        if(blendMode<.5)W=sceneUnder;
        if(blendMode>.5 && a>.02 && a<.995)S=clamp((sceneUnder-(blendMode<1.5?own.rgb*a:own.rgb))/(1.-a),0.,1.5);
        // The game's colour of its water WITHOUT its texture: the water colour's mip chain (water_color_mips) at the level whose texels cover ~6
        // yards of water (the pixel's footprint across the view, where it is not stretched), read as a cubic B-spline between two levels, divided
        // by the averaged alpha because the chain also averages the empty texels around the water. MEASURED on 32267-2 / 13655-5 / 56682-1: the
        // texture's luminance spread falls 4-6x by a 4-yard window, what stays beyond is the game's broad light (its glow toward the sun), kept.
        // A fixed screen level (1/17 of the height, 2026-10-08 first build) left the texture as large blurry blocks seen from above (32267, in
        // game). Keeps the zone tint and the time of day. Without a chain (one level) or with an unknown blend: the game's own colour.
        vec3 tint=W;
        int tintLevels=textureQueryLevels(waterColor);
        if(tintLevels>1 && blendMode>.5) {
            float level=clamp(log2(6./max(footprint*cosView,1e-4)),2.,float(tintLevels-1));
            int l0=int(floor(level)),l1=min(l0+1,tintLevels-1);
            vec2 at=(vec2(under)+.5)/extent;
            vec4 m=mix(textureBSpline(waterColor,at,l0),textureBSpline(waterColor,at,l1),level-float(l0));
            tint=m.rgb/max(m.a,1e-4)*(blendMode>1.5?1.:a);
        }
        if(debug==21){result=vec4(tint,1);return;}                                                   // WATER TINT
        // caustics on the bed: light the surface focuses, strongest in shallow water, gone in deep water and far away (no light below ~6 yards
        // in WoW's clear water would be wrong physically, but the pattern would only shimmer there); with the sun, stronger in sunlight
        vec3 causticLight=vec3(0.);
        if(((layers&4)!=0 || debug==20) && params.fx.y>0. && depthBelow<7. && water.z<110.) {
            vec2 ndcB=(gl_FragCoord.xy-viewport.xy)/viewport.zw*2.-1.;
            vec3 bedWorld=rawHere<range.y-1e-6?worldOf(vec3(ndcB/projection.zw,1.)*linearDepth(rawHere,range,coefficients),c5,c6,c7,c8):hitWorld;
            // Only where light can focus (HEURISTIC, 48451-2 in game): not in the first ~yard of water (the light has not converged yet; at
            // the waterline they were a bright orange rim) and not on steep beds - the pattern is world-xy anchored, so on a rock face under
            // the water it ran down in vertical streaks. The bed's slope comes from the scene depth around the pixel.
            float shallowBed=smoothstep(6.5,.4,depthBelow)*smoothstep(.15,1.,depthBelow)*(1.-smoothstep(55.,110.,water.z))*bedFlatness(here,extent,range,projection,viewport,coefficients,up);
            float daylight=params.sun.w>.5?.6+.8*clamp(params.sun.z*2.,0.,1.):clamp(dot(skyColor(c7,c5,c6,c7),vec3(.299,.587,.114))*1.6,.25,1.);
            causticLight=modernCaustics(bedWorld.xy,depthBelow,time,footprint,params.sun.xyz,params.sun.w>.5)*(params.fx.y*1.5*shallowBed*daylight);   // x1.5 (was 2.4: black / white beds, 48451-2): TUNING
            // Focused light redistributes, it does not blacken: at most 45% darker, 120% brighter (the full strength turned beds black in spots).
            causticLight=clamp(causticLight,vec3(-.45),vec3(1.2));
            S*=1.+causticLight;
        }
        // ---- the water body (layer 4): Beer-Lambert per channel along the view ray, between the bed without any water (T = 1) and the game's
        // colour of its water without its texture (T = 0: the zone tint above). Before 2026-10-08 the deep end was the game's whole look (its
        // texture included), and its sparkle pattern showed through everywhere (13655-1). Red goes first: shallow water turns clear blue-green over
        // the bed (Crest / LesusX use the same law). ----
        vec3 sigma=params.body.rgb*mix(8.,1.,clamp(params.fx.x,0.,1.));        // clarity 1 = pure water, 0 = 8x murkier (TUNING)
        vec3 T=exp(-sigma*pathLength);
        vec3 scatter=tint;
        if(params.tone.w>0.){float luma=dot(scatter,vec3(.299,.587,.114));scatter=mix(scatter,params.tone.rgb*(luma*1.5+.06),params.tone.w*.6);}
        vec3 body=(layers&4)!=0?S*T+scatter*(1.-T):sceneUnder;
        vec3 Lv=normalize(c5*params.sun.x+c6*params.sun.y+c7*params.sun.z);
        bool sunOk=params.sun.w>.5;
        // subsurface light: looking toward the sun, the thin high water of the crests glows (Crest's SSS term; strengths TUNING)
        if(sunOk && (layers&4)!=0 && params.body.w>0.) {
            float towardSun=pow(max(0.,dot(Lv,-V)),4.);
            float crest=clamp(wave.displacement.z/max(params.surface.y,1e-4)*.5+.5,0.,1.);
            body+=tint*params.sunColor.rgb*(1.2*params.body.w*towardSun*crest*(1.-pow(abs(dot(V,up)),2.)));   // lit through the water's own colour
        }
        // ---- reflection: the planar mirror (mode 2, the validated level: exact) where it has something, the screen-space march for what it lacks
        // (and everywhere in SSR mode), the sky memory seen through the roughness where neither hits ----
        vec3 direction=reflect(-V,nF);
        float du=dot(direction,up);if(du<.02)direction=normalize(direction+up*(.02-du));   // never below the horizon (the sky memory has no lower half)
        vec3 env=skyRough(direction,alpha,c5,c6,c7),origin=surfaceView+up*.035;
        vec3 planar=env,ssr=env;float planarWeight=0.,ssrWeight=0.;
        if(params.options.x>1.5 && onPlane) {
            vec2 shifted=clamp(uv+(nF.xy-up.xy)*.05/(1.+water.z*.02),vec2(.001),vec2(.999));
            vec2 texel=vec2(1.)/extent*(1.5+water.z*.02+alpha*30.);
            vec3 sum=vec3(0.);float count=0.;
            for(int t=0;t<5;++t) {
                vec2 offset=t==0?vec2(0.):t==1?vec2(texel.x,0.):t==2?vec2(-texel.x,0.):t==3?vec2(0.,texel.y):vec2(0.,-texel.y);
                vec2 at=clamp(shifted+offset,vec2(.001),vec2(.999));
                if(texture(planarDepth,at).r<.99999){sum+=texture(planarColor,at).rgb;count+=1.;}
            }
            if(count>0.){planar=sum/count;planarWeight=count/5.;}
        }
        if(((layers&1)!=0 || debug==4 || (debug>=13 && debug<=15)) && planarWeight<.999) {
            vec2 w;float travel;
            if(ssrMarch(origin,direction,extent,range,projection,viewport,coefficients,w,travel)) {
                ssr=softScene(ivec2(w),1.+travel*(.02+alpha*.5),ivec2(extent)-1);
                vec2 edge=min(w,extent-w)/extent;
                ssrWeight=smoothstep(0.,.08,min(edge.x,edge.y))*(1.-smoothstep(.35,.7,alpha));
            }
        }
        vec3 reflected=expandBright(mix(mix(env,ssr,ssrWeight),planar,planarWeight));
        if(debug==4){result=vec4(reflected,1);return;}                                               // REFLECTION (as blended in)
        vec3 color=body;
        if((layers&1)!=0)color=mix(body,reflected,clamp(fresnel*params.options.y,0.,1.));
        // ---- the sun (layer 1): GGX lobe on the full multi-scale normal; with the fine ripples resolved close up it breaks into sparse glints,
        // far away the averaged ripples widen it into the glitter path (radiance x pi at the peak; strength = SUN GLITTER, TUNING) ----
        vec3 sunLight=vec3(0.);
        if(sunOk && ((layers&1)!=0 || debug==16)) {
            vec3 H=normalize(Lv+V);
            float NL=max(dot(nF,Lv),0.),NH=max(dot(nF,H),0.),VH=max(dot(V,H),0.);
            float a2=alpha*alpha,dd=NH*NH*(a2-1.)+1.,D=a2/(3.14159265*dd*dd);
            float vis=.5/max(NL*sqrt(NV*NV*(1.-a2)+a2)+NV*sqrt(NL*NL*(1.-a2)+a2),1e-4);
            float F=R0+(1.-R0)*pow(1.-VH,5.);
            float intensity=params.sunColor.w*D*vis*F*NL*3.14159265;
            // Sparse glints (HEURISTIC): world-anchored .4-yard cells twinkle over time; resolved close up, averaged to 1 where a pixel covers a
            // cell (no screen-space shimmer). The path breaks into sparkles instead of one lit patch.
            float fade=1.-smoothstep(.3,1.2,footprint/.4);
            float twinkle=valueNoise(worldXY/.4+vec2(time*.9,time*.6))*.6+valueNoise(worldXY/.17-vec2(time*.7,-time*1.1))*.4;
            intensity*=mix(1.,pow(twinkle,5.)*14.,fade*.85);
            // Soft shoulder that keeps the sun's hue: an LDR clip turned the orange path into flat yellow-white.
            sunLight=params.sunColor.rgb*(1.-exp(-intensity*.35))*1.35;
        }
        color+=sunLight;
        if((layers&8)!=0)color=applyFoam(color,worldXY,depthBelow,footprint,water.z,env,modernDetail,
                                         params.fx.w>0. && water.z<220.?jacobianCrest(worldXY,footprint)*min(params.fx.w*1.25,1.5):0.);
        if(debug==8){result=vec4(clamp(.5+medium*2.5,0.,1.),.5,1);return;}                          // MEDIUM NORMALS: rg = slope x2.5
        if(debug==9){result=vec4(clamp(.5+micro*2.5,0.,1.),.5,1);return;}                           // MICRO NORMALS
        if(debug==10){result=vec4(normalize(vec3(-slopeF,1.))*.5+.5,1);return;}                      // FINAL NORMAL (world)
        if(debug==11){result=vec4(vec3(sqrt(fresnel)),1);return;}                                    // FRESNEL (sqrt: the small values show)
        if(debug==12){result=vec4(vec3(alpha*2.),1);return;}                                         // ROUGHNESS (GGX alpha x2)
        if(debug==13){result=vec4(vec3(ssrWeight*(1.-planarWeight)),1);return;}                      // SSR CONFIDENCE (as used)
        if(debug==14){result=vec4(vec3(planarWeight),1);return;}                                     // PLANAR WEIGHT
        if(debug==15){result=vec4(vec3((1.-ssrWeight)*(1.-planarWeight)),1);return;}                 // ENVIRONMENT WEIGHT
        if(debug==16){result=vec4(sunLight,1);return;}                                               // SUN GLINTS
        if(debug==17){result=vec4(vec3(1.-exp(-pathLength/10.)),1);return;}                          // WATER THICKNESS (path in the water, 10-yd scale)
        if(debug==18){result=vec4(1.-T,1);return;}                                                   // ABSORPTION (1 - transmittance per channel)
        if(debug==19){result=vec4(clamp(refractPx/20.,0.,1.),under==here?0.:1.,0.,1);return;}          // REFRACTION: r = shift / 20 px, g = used
        if(debug==20){result=vec4(clamp(.5+causticLight,0.,1.),1);return;}                           // CAUSTICS (.5 = none)
        result=vec4(color,1.);return;
    }
    vec3 base=texelFetch(sceneColor,here,0).rgb,color=base;
    // Opacity of the game's own water texture (its pattern and its own foam lines): below 1 it is blended toward a smooth version of itself, the
    // average of two rings of neighbouring WATER pixels (never sand or objects), so the layers below take over. The ring radius is about 1.4 yards (at most 44 px).
    float gameOpacity=clamp(params.extra.x,0.,1.);
    if(gameOpacity<.999) {
        // Eight taps on one ring, rotated a little per pixel so the sampling pattern does not band; a tap counts only if the water shader drew
        // there (its own alpha, one 8-byte fetch instead of the 16-byte mask).
        float radius=clamp(1.4*projection.w*extent.y/(2.*water.z),3.,44.);
        const vec2 ring[8]=vec2[8](vec2(1.,0.),vec2(.7071,.7071),vec2(0.,1.),vec2(-.7071,.7071),vec2(-1.,0.),vec2(-.7071,-.7071),vec2(0.,-1.),vec2(.7071,-.7071));
        float turn=fract(dot(gl_FragCoord.xy,vec2(.06711056,.00583715)));turn=fract(52.9829189*turn)*.7854;       // per-pixel angle in 0..45 degrees
        vec2 rotation=vec2(cos(turn),sin(turn));
        vec3 sum=base;float count=1.;
        for(int i=0;i<8;++i) {
            vec2 offset=ring[i]*radius;offset=vec2(offset.x*rotation.x-offset.y*rotation.y,offset.x*rotation.y+offset.y*rotation.x);
            ivec2 tap=ivec2(clamp(gl_FragCoord.xy+offset,vec2(0.),extent-1.));
            if(texelFetch(waterColor,tap,0).a>0.){sum+=texelFetch(sceneColor,tap,0).rgb;count+=1.;}
        }
        color=mix(sum/count,base,gameOpacity);
    }
    // Water depth under this pixel: the world height of the surface the view ray reaches below the water (open sea with nothing drawn = deep).
    float depthBelow=40.;
    if(rawHere<range.y-1e-6) {
        vec2 ndc=(gl_FragCoord.xy-viewport.xy)/viewport.zw*2.-1.;
        vec3 seabed=vec3(ndc/projection.zw,1.)*linearDepth(rawHere,range,coefficients);
        depthBelow=max(0.,hitWorld.z-dot(seabed-c8,c7));   // from the moving surface
    }
    // ---- seabed clarity and caustics. The scene image holds the game's blend of its water over the seabed: scene = a*W + (1-a)*S (or premultiplied).
    // The mask pass kept the water's own color W and alpha a, so the seabed S is recovered, lit by caustics, and blended back with a smaller alpha in
    // shallow water: the bed shows through. With no blend information (mode 0) the bed is left alone and caustics lighten the water itself. ----
    float clarityAmount=params.fx.x,causticAmount=params.fx.y,blendMode=params.extra.y;
    float causticLight=0.;                                                          // the bed (or the water) is brightened by 1 + causticLight
    if((layers&4)!=0 && causticAmount>0. && depthBelow<5. && water.z<90.) {
        float shallowBed=smoothstep(3.4,.3,depthBelow)*(1.-smoothstep(45.,90.,water.z));
        float daylight=clamp(dot(skyColor(c7,c5,c6,c7),vec3(.299,.587,.114))*1.6,.25,1.);
        causticLight=causticAmount*1.6*caustic(worldXY,time,footprint)*shallowBed*daylight;
    }
    bool bedShown=false;
    if((layers&4)!=0 && clarityAmount>0. && blendMode>.5 && depthBelow<7.) {
        vec4 own=texelFetch(waterColor,here,0);
        float a=clamp(own.a,0.,1.);
        if(a>.02 && a<.995) {
            vec3 ownAdded=blendMode<1.5?own.rgb*a:own.rgb;                          // what the water contributed to the scene pixel
            vec3 bed=clamp((base-ownAdded)/(1.-a),0.,1.5);
            float lifted=clarityAmount*exp(-depthBelow*.42)*(1.-smoothstep(60.,140.,water.z));   // how much of the water's opacity is removed
            float aNew=a*(1.-.9*lifted);
            bed*=mix(vec3(1.),vec3(.86,.94,.98),1.-exp(-depthBelow*.5));            // deeper bed: a little darker and bluer
            bed*=1.+causticLight*1.4;                                                // caustics live on the bed
            color+=ownAdded*(aNew/a)+bed*(1.-aNew)-base;
            bedShown=true;
        }
    }
    if(!bedShown && causticLight>0.)color*=1.+causticLight*.6;
    // ---- refraction wobble: shallow water shows its bottom, shifted by the large waves' tilt (only from other water pixels; the short ripples
    // would move the picture by a few pixels and read as grain) ----
    if((layers&2)!=0) {
        vec2 tilt=(c5*-largeSlope.x+c6*-largeSlope.y).xy;
        vec2 shifted=gl_FragCoord.xy+tilt*extent.y*.14*clamp(depthBelow*.5,0.,1.);
        ivec2 other=ivec2(clamp(shifted,vec2(0.),extent-1.));
        vec4 otherWater=texelFetch(waterMask,other,0);
        if(otherWater.a>0. && texelFetch(sceneDepth,other,0).r+2e-5>=mix(range.x,range.y,coefficients.z+coefficients.w/otherWater.z))
            color=mix(color,softScene(other,1.,ivec2(extent)-1),.75);
    }
    // ---- depth tint: deeper water absorbs toward a blue-green ----
    if((layers&4)!=0) {
        float thickness=1.-exp(-depthBelow*.10);
        color=mix(color,vec3(.03,.20,.24),thickness*.30);
        // Tone: the water takes the chosen color while keeping its own light and dark (luma preserved), more so where it is deep.
        if(params.tone.w>0.) {
            float luma=dot(color,vec3(.299,.587,.114));
            color=mix(color,params.tone.rgb*(luma*1.5+.06),params.tone.w*(.30+.45*thickness));
        }
    }
    // ---- reflection: screen-space march / planar mirror, with the sky memory wherever there is nothing to hit ----
    vec3 incident=normalize(surfaceView),direction=reflect(incident,normal);
    vec3 origin=surfaceView+flatNormal*.035;
    vec3 sky=skyColor(direction,c5,c6,c7);
    vec3 reflected=sky;float confidence=0.;
    if(params.options.x>1.5 && onPlane) {   // the planar image mirrors the validated level only; other levels use the screen-space march
        vec2 shifted=clamp(uv+(normal.xy-flatNormal.xy)*.05/(1.+water.z*.02),vec2(.001),vec2(.999));
        vec2 texel=vec2(1.)/extent*(1.5+water.z*.02);
        vec3 sum=vec3(0.);float count=0.;
        for(int t=0;t<5;++t) {
            vec2 offset=t==0?vec2(0.):t==1?vec2(texel.x,0.):t==2?vec2(-texel.x,0.):t==3?vec2(0.,texel.y):vec2(0.,-texel.y);
            vec2 at=clamp(shifted+offset,vec2(.001),vec2(.999));
            if(texture(planarDepth,at).r<.99999){sum+=texture(planarColor,at).rgb;count+=1.;}
        }
        if(count>0.){reflected=sum/count;confidence=count/5.;reflected=mix(sky,reflected,confidence);}
    } else if((layers&1)!=0 || debug==4) {   // the march is the expensive part: only when the reflection is shown (planar: other water levels)
        vec2 w;float travel;
        if(ssrMarch(origin,direction,extent,range,projection,viewport,coefficients,w,travel)) {
            reflected=softScene(ivec2(w),1.+travel*.03,ivec2(extent)-1);
            vec2 edge=min(w,extent-w)/extent;
            confidence=smoothstep(0.,.08,min(edge.x,edge.y));      // fades into the sky memory at the picture's edge
        }
        reflected=mix(sky,reflected,confidence);
    }
    // Life: a little more saturation, bright reflections (sun, sky glow) sparkle, and the tone flavors the reflection too.
    float reflectedLuma=dot(reflected,vec3(.299,.587,.114));
    reflected=mix(vec3(reflectedLuma),reflected,1.25);
    reflected*=1.+.9*smoothstep(.55,1.,reflectedLuma);
    reflected=mix(reflected,reflected*(vec3(.75)+params.tone.rgb*.7),params.tone.w*.5);
    if(debug==4){result=vec4(reflected,1);return;}
    if((layers&1)!=0) {
        float fresnel=.02+.98*pow(1-clamp(dot(normal,normalize(-water.xyz)),0,1),5);
        color=mix(color,reflected,clamp(clamp(water.a,0,1)*params.options.y*(.30+.70*fresnel),0,1));
    }
    // ---- sun glitter: the reflection again with fine ripple slopes. Where the reflected direction lands on the bright part of the sky memory (the
    // sun and its glow) the tiny facets flash. Sparkles are mip-filtered away with distance. ----
    float glitterAmount=params.fx.z;
    if((layers&1)!=0 && glitterAmount>0. && water.z<220.) {
        vec2 micro=glintSlope(worldXY,time,footprint);
        vec3 facet=normalize(c5*-(slope.x+micro.x*.9)+c6*-(slope.y+micro.y*.9)+c7);
        if(dot(facet,-water.xyz)<0)facet=-facet;
        vec3 glintDirection=reflect(incident,facet);
        float luma=dot(skyColor(glintDirection,c5,c6,c7),vec3(.299,.587,.114));
        float grazing=.02+.98*pow(1.-clamp(dot(normal,normalize(-water.xyz)),0.,1.),5.);
        float spark=smoothstep(.55,.95,luma)*smoothstep(0.,.06,dot(glintDirection,c7));
        color+=vec3(1.,.93,.80)*spark*glitterAmount*.9*(.25+.75*grazing)*(1.-smoothstep(80.,220.,water.z));
    }
    // ---- foam. Method: the local foam AMOUNT (0..1) is a coverage, and a fine lace texture whose values are uniformly distributed is cut at
    // 1-amount with a narrow edge (the "noise cutoff scaled by a depth gradient" of toon/shore shaders). Where the amount is low only a thin lace net
    // survives, where it is high the foam is solid with round holes, and the edge of every patch is lacy instead of a soft blob.
    // The amount comes from (1) a shore envelope from the water depth, (2) swash fronts running in toward the shore, sharp on the shore side with
    // the lace trailing out to sea, (3) a drifting, warped patch field that tears the fronts apart, (4) a per-patch rhythm so foam comes and goes,
    // (5) a thin foam line at the water's edge that is only kept close up: from afar it would read as a straight white line.
    // Everything drifts together with one velocity (foam moves with the water, it does not boil). Deep water costs nothing unless the amount asks
    // for open-water foam. ----
    if((layers&8)!=0)color=applyFoam(color,worldXY,depthBelow,footprint,water.z,sky,waterDetail,-1.);
    result=vec4(color,1.);
}
