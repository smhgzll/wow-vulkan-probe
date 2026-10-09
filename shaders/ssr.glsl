// General experimental SSR, finite manual projection. No water classification.
struct ReflectionHit { bool valid; vec2 uv; float weight; };
ReflectionHit traceReflection(vec2 p) {
    ReflectionHit hit=ReflectionHit(false,vec2(0),0.0);
    if(getDepth(p)>=.99999)return hit;
    vec3 position=viewPosition(p), normal=viewNormal(p);
    vec3 incident=fx.depthConfig.y==2u?vec3(0,0,-1):normalize(position);
    vec3 direction=reflect(incident,normal);
    vec3 origin=position+normal*max(fx.ssrRay.y*.5,.001);
    if(!insideScreen(projectView(origin)))return hit;
    vec3 previous=origin;
    float previousDelta=-origin.z-linearDepth(clamp(projectView(origin),vec2(0),vec2(1)));
    int count=clamp(int(fx.ssrConfig.y),16,32);
    for(int i=0;i<32;i++) {
        if(i>=count)break;
        float travel=float(i+1)*fx.ssrRay.x;
        if(travel>fx.ssrRay.z)break;
        vec3 point=origin+direction*travel;
        if(point.z>=-fx.projection.x || -point.z>fx.projection.y)break;
        vec2 q=projectView(point);
        if(!insideScreen(q))break;
        if(getDepth(q)>=.99999){previous=point;previousDelta=-1.0;continue;}
        float delta=-point.z-linearDepth(q);
        if(i>0 && delta>=0.0 && previousDelta<=0.0) {
            vec3 low=previous,high=point;
            for(int refine=0;refine<5;refine++) {
                vec3 middle=(low+high)*.5;
                vec2 midUV=projectView(middle);
                if(getDepth(midUV)>=.99999 || -middle.z<linearDepth(midUV))low=middle;else high=middle;
            }
            q=projectView(high);delta=-high.z-linearDepth(q);
            // Reject large discontinuity crossings: they are silhouette misses.
            if(delta>=0.0 && delta<=fx.ssrRay.y && distance(q,p)>1.5/max(fx.viewport.x,fx.viewport.y)) {
                float edge=min(min(q.x,1.0-q.x),min(q.y,1.0-q.y));
                hit=ReflectionHit(true,q,smoothstep(0.0,fx.ssrRay.w,edge)*(1.0-travel/fx.ssrRay.z));
                return hit;
            }
        }
        previous=point;previousDelta=delta;
    }
    return hit;
}
