// View-space hemisphere AO. No additional image, submit, or synchronization.
float hemisphereAO(vec2 p) {
    if (getDepth(p)>=.99999) return 0.0;
    vec3 center=viewPosition(p), normal=viewNormal(p);
    vec3 tangent=normalize(cross(abs(normal.z)<.999?vec3(0,0,1):vec3(0,1,0),normal));
    vec3 bitangent=cross(normal,tangent);
    float noise=fract(sin(dot(floor(p*fx.viewport.xy),vec2(12.9898,78.233)))*43758.5453);
    int count=clamp(int(fx.aoConfig.y),8,16);
    float occluded=0.0;
    for(int i=0;i<16;i++) {
        if(i>=count) break;
        float t=(float(i)+.5)/float(count);
        float ct=.1+.9*fract(float(i)*.61803398875+.5);
        float st=sqrt(1.0-ct*ct);
        float angle=6.2831853*(float(i)*.61803398875+noise);
        vec3 direction=normal*ct+(tangent*cos(angle)+bitangent*sin(angle))*st;
        vec3 query=center+direction*fx.ssao.y*(.1+.9*t*t);
        if(query.z>=-fx.projection.x) continue;
        vec2 sampleUV=projectView(query);
        if(!insideScreen(sampleUV)||getDepth(sampleUV)>=.99999) continue;
        float surfaceZ=linearDepth(sampleUV);
        float attenuation=1.0-smoothstep(0.0,fx.ssao.y,abs(surfaceZ+center.z));
        occluded += surfaceZ < -query.z-fx.ssao.z ? attenuation : 0.0;
    }
    return occluded/float(count);
}
float computeSSAO(vec2 p) {
    float ao=hemisphereAO(p);
    if(fx.aoConfig.z!=0u) {
        float z=linearDepth(p), total=1.0;
        vec2 px=1.5/fx.viewport.xy;
        const vec2 offsets[4]=vec2[](vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1));
        for(int i=0;i<4;i++) {
            vec2 q=p+offsets[i]*px;
            if(!insideScreen(q)||getDepth(q)>=.99999)continue;
            float dz=abs(linearDepth(q)-z);
            if(dz>fx.ssao.y*.15)continue;
            float weight=exp(-dz/max(fx.ssao.y*.05,1e-5));
            ao+=hemisphereAO(q)*weight;total+=weight;
        }
        ao/=total;
    }
    return clamp(ao*fx.ssao.x,0.0,1.0);
}
