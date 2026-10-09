// Manual, finite symmetric projection only. No automatic WoW matrix claim.
bool linearDepthReady() {
    return fx.depthConfig.x != 0u && (fx.depthConfig.y == 1u || fx.depthConfig.y == 2u) &&
           fx.projection.x >= .0001 && fx.projection.y > fx.projection.x;
}
float linearDepth(vec2 p) {
    float d = getDepth(p); // explicit convention normalized to near=0, far=1
    float n = fx.projection.x, f = fx.projection.y;
    return fx.depthConfig.y == 2u ? mix(n,f,d) : n/((1.0-d)+d*(n/f));
}
bool positionReady() {
    return linearDepthReady() && (fx.depthConfig.y == 1u ? fx.projection.z > 0.0 && fx.projection.z < 179.0 : fx.projection.w > 0.0);
}
vec2 projectionScale() {
    float halfHeight = fx.depthConfig.y == 1u ? tan(radians(fx.projection.z)*0.5) : fx.projection.w*0.5;
    return vec2(halfHeight*fx.viewport.x/fx.viewport.y, halfHeight);
}
// Right-handed view: +X right, +Y up, visible points have negative Z.
vec3 viewPosition(vec2 p) {
    float z = linearDepth(p);
    vec2 xy = vec2(p.x*2.0-1.0, 1.0-p.y*2.0)*projectionScale();
    if (fx.depthConfig.y == 1u) xy *= z;
    return vec3(xy,-z);
}
vec2 projectView(vec3 p) {
    vec2 xy = p.xy/projectionScale();
    if (fx.depthConfig.y == 1u) xy /= -p.z;
    return vec2(xy.x,-xy.y)*0.5+0.5;
}
bool insideScreen(vec2 p) { return all(greaterThanEqual(p,vec2(0))) && all(lessThanEqual(p,vec2(1))); }
vec3 viewNormal(vec2 p) {
    vec2 px = 1.0/vec2(textureSize(depthTexture,0));
    vec3 center=viewPosition(p);
    vec3 right=viewPosition(clamp(p+vec2(px.x,0),vec2(0),vec2(1)))-center;
    vec3 left=center-viewPosition(clamp(p-vec2(px.x,0),vec2(0),vec2(1)));
    vec3 up=viewPosition(clamp(p-vec2(0,px.y),vec2(0),vec2(1)))-center;
    vec3 down=center-viewPosition(clamp(p+vec2(0,px.y),vec2(0),vec2(1)));
    vec3 dx=abs(right.z)<abs(left.z)?right:left;
    vec3 dy=abs(up.z)<abs(down.z)?up:down;
    vec3 normal=cross(dx,dy);
    float norm=dot(normal,normal);
    return norm>1e-16 ? normal*inversesqrt(norm) : vec3(0,0,1);
}
