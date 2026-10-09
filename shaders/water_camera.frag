#version 450
// Exact known WoW VS float-register window: c0..c3=P, c5..c8=M.
layout(std140,set=0,binding=0) uniform Source { vec4 c[9]; } source;
layout(std430,set=0,binding=1) readonly buffer Vertices { uint words[]; } vertices;
layout(std430,set=0,binding=2) readonly buffer Indices { uint words[]; } indices;
layout(std140,set=0,binding=3) uniform Parameters {
    vec4 viewport; vec4 depthRange; uvec4 vertex; uvec4 indices; ivec4 draw; vec4 options; vec4 tone; vec4 foam; vec4 look; vec4 extra; vec4 fx;
} params;
layout(location=0) out vec4 result;
bool finite4(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool positionAt(uint ordinal,out vec3 position,out bool restart) {
    restart=false;uint index=params.indices.z+ordinal;
    if(params.indices.y!=0u) {
        uint address=params.indices.x+index*params.indices.y;
        uint word=indices.words[address/4u];
        index=params.indices.y==2u?(word>>((address&2u)*8u))&65535u:word;
        restart=params.draw.y!=0 && index==(params.indices.y==2u?65535u:0xffffffffu);
        if(restart)return true;
        if(index>0x7fffffffu)return false;
        if(params.draw.x<0) {uint negative=uint(-(params.draw.x+1))+1u;if(index<negative)return false;index-=negative;}
        else {if(index>0x7fffffffu-uint(params.draw.x))return false;index+=uint(params.draw.x);}
    }
    if(params.vertex.x>params.vertex.z || params.vertex.w>params.vertex.z-params.vertex.x ||
       index>(params.vertex.z-params.vertex.x-params.vertex.w)/params.vertex.y)return false;
    uint address=(params.vertex.x+index*params.vertex.y)/4u;
    position=uintBitsToFloat(uvec3(vertices.words[address],vertices.words[address+1u],vertices.words[address+2u]));
    return finite4(vec4(position,1));
}
void main() {
    mat4 p=mat4(source.c[0],source.c[1],source.c[2],source.c[3]);
    mat4 m=mat4(source.c[5],source.c[6],source.c[7],source.c[8]);
    bool valid=true;
    for(int i=0;i<4;++i)valid=valid && finite4(p[i]) && finite4(m[i]);
    valid=valid && p[0].x>0 && p[1].y>0 && p[2].z>1 && p[3].z<0 && abs(p[2].w-1)<1e-5;
    valid=valid && length(p[0].yzw)<1e-6 && length(p[1].xzw)<1e-6 && length(p[2].xy)<1e-6 && length(p[3].xyw)<1e-6;
    valid=valid && length(vec3(m[0].w,m[1].w,m[2].w))<1e-6 && abs(m[3].w-1)<1e-6;
    mat3 gram=transpose(mat3(m))*mat3(m);
    for(int i=0;i<3;++i)valid=valid && length(gram[i]-mat3(1)[i])<1e-4;
    // Two validities. `valid` (texel 13.x): the camera is sane and the selected water draw is readable - enough for the
    // mask (the instrumented water FS multiplies its alpha by it), the per-pixel water levels in the composite, the sky
    // memory and the fog's water-surface stop. `plane` (texel 18.x): a reference level for the planar mirror.
    // The reference level is the first horizontal run of three vertices BELOW the camera. One WoW draw can hold several
    // levels (Shadowglen: ponds at 1306.97 and 1308.36), and the first draw of a pass need not be the sea at all (Booty
    // Bay, capture 39521-2: pools up the hill at z 45.8/84.8/104 above a camera at z 14.6). Requiring the first level of
    // the first draw used to reject the whole frame: no mask, no water look, fog down to the seabed, depending on the view.
    // All referenced vertices must still be readable and finite. No guessed sea level, no CPU snapshot of an older frame.
    mat4 invP=mat4(1),invM=mat4(1),reflection=mat4(1);vec4 planeClip=vec4(0),planeView=vec4(0);
    float cameraZ=0;
    if(valid){invP=inverse(p);invM=inverse(m);cameraZ=invM[3].z/invM[3].w;}
    float height=0;bool plane=false;
    float last=0;int run=0;
    for(uint i=0u;i<params.indices.w && valid;++i) {
        vec3 point;bool restart;
        valid=positionAt(i,point,restart);
        if(!valid || restart){run=0;continue;}
        run=run>0 && abs(point.z-last)<=.005?run+1:1;
        last=point.z;
        if(!plane && run>=3 && point.z<cameraZ-.001){height=point.z;plane=true;}
    }
    valid=valid && params.depthRange.y>params.depthRange.x;
    plane=plane && valid;
    if(valid) {
        // planeView.xyz (world up in view space) is needed by every level; with no reference level height stays 0 and only
        // the planar mirror (gated by `plane`) would be wrong.
        mat4 mirror=mat4(1);mirror[2].z=-1;mirror[3].z=2*height;
        reflection=p*m*mirror*invM*invP;
        planeView=transpose(invM)*vec4(0,0,1,-height);
        planeClip=transpose(invP)*planeView;
    }
    int column=int(gl_FragCoord.x);
    result=vec4(0);
    if(column<4)result=reflection[column];
    else if(column<8)result=invP[column-4];
    else if(column==8)result=planeClip;
    else if(column==9)result=planeView;
    else if(column==10)result=vec4(-p[3].z/p[2].z,-p[3].z/(p[2].z-1),p[0].x,p[1].y);
    else if(column==11)result=params.viewport;
    else if(column==12)result=params.depthRange;
    else if(column==13)result=vec4(valid?1:0,height,p[2].z,p[3].z);
    else if(column>=14 && column<18)result=m[column-14];   // view matrix columns c5..c8: world -> view
    else if(column==18)result=vec4(plane?1:0,height,cameraZ,0);
}
