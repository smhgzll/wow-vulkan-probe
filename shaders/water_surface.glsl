// The water surface: GPU mirror of src/water_surface.cpp (keep identical; the definition and why it is evaluated per pixel are in
// water_surface.hpp). Base plane + Gerstner waves in world space (yards, z up). The including shader declares the `params` block with
// waveA[5] (direction xy, k, amplitude), waveB[5] (omega, Q, phase now, 0) and surface (active waves, sum A, sum QkA, sum kA).
struct WaterSurface {
    vec3 position;       // world, on the displaced surface
    vec3 normal;         // world, unit, up
    vec2 parameter;      // rest position x0: textures and foam ride the waves with it
    vec3 displacement;   // P - (x0, base)
    float jacobian;      // horizontal compression: 1 flat, < 1 crests, 0 folding
    float sxx,syy,sxy;   // dP/dx0 = (1 - sxx, -sxy, slope.x), dP/dy0 = (-sxy, 1 - syy, slope.y)
    vec2 slope;
};
float waterWaveLod(float k,float footprint) { return footprint>0.?smoothstep(3.,8.,6.2831853/(k*footprint)):1.; }
// delay (seconds) evaluates the surface as it was that long ago (phase(t - delay) = phase(t) + omega * delay).
WaterSurface waterSurfaceAtTime(vec2 x0,float baseZ,float footprint,float delay) {
    float dx=0.,dy=0.,dz=0.,sxx=0.,syy=0.,sxy=0.,cx=0.,cy=0.;
    int n=int(params.surface.x);
    for(int i=0;i<n;++i) {
        vec4 a=params.waveA[i],b=params.waveB[i];
        float A=a.w*waterWaveLod(a.z,footprint),Q=b.y;
        float theta=a.z*(a.x*x0.x+a.y*x0.y)+b.z+b.x*delay,s=sin(theta),c=cos(theta);
        dx+=Q*A*a.x*c;dy+=Q*A*a.y*c;dz+=A*s;
        float kA=a.z*A;
        sxx+=Q*kA*a.x*a.x*s;syy+=Q*kA*a.y*a.y*s;sxy+=Q*kA*a.x*a.y*s;
        cx+=kA*a.x*c;cy+=kA*a.y*c;
    }
    WaterSurface o;
    o.sxx=sxx;o.syy=syy;o.sxy=sxy;o.slope=vec2(cx,cy);
    o.parameter=x0;o.displacement=vec3(dx,dy,dz);o.position=vec3(x0+vec2(dx,dy),baseZ+dz);
    o.jacobian=(1.-sxx)*(1.-syy)-sxy*sxy;
    vec3 n3=vec3(-(cx*(1.-syy)+sxy*cy),-(cy*(1.-sxx)+sxy*cx),o.jacobian);
    float len=length(n3);
    o.normal=len>1e-12?n3/len:vec3(0.,0.,1.);
    return o;
}
WaterSurface waterSurfaceAt(vec2 x0,float baseZ,float footprint) { return waterSurfaceAtTime(x0,baseZ,footprint,0.); }
// The displaced surface along a view ray (world; d points down): Newton's method on x0 from the base-plane hit (water_surface.hpp).
// hit = the ray's point at the surface height (what the pixel sees). False when the ray does not go down to the base plane.
bool waterSurfaceHit(vec3 o,vec3 d,float baseZ,float footprint,out WaterSurface s,out vec3 hit) {
    s=waterSurfaceAt(o.xy,baseZ,1e9);hit=o;
    if(!(d.z< -1e-6))return false;
    float t=(baseZ-o.z)/d.z;
    if(!(t>0.))return false;
    vec2 x0=o.xy+t*d.xy,r=d.xy/d.z;
    for(int i=0;i<3;++i) {
        WaterSurface e=waterSurfaceAt(x0,baseZ,footprint);
        t=(e.position.z-o.z)/d.z;
        vec2 f=e.position.xy-(o.xy+t*d.xy);
        float m00=1.-e.sxx-r.x*e.slope.x,m01=-e.sxy-r.x*e.slope.y,m10=-e.sxy-r.y*e.slope.x,m11=1.-e.syy-r.y*e.slope.y,det=m00*m11-m01*m10;
        if(abs(det)>.05)x0-=vec2(m11*f.x-m01*f.y,m00*f.y-m10*f.x)/det;
        else x0-=f;
    }
    s=waterSurfaceAt(x0,baseZ,footprint);
    t=(s.position.z-o.z)/d.z;
    hit=vec3(o.xy+t*d.xy,s.position.z);
    return true;
}
