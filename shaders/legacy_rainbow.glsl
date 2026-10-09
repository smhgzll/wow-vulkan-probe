float legacyOcclusion=0.0;
vec3 hsv2rgb(vec3 c)
{
    vec3 rgb = clamp(
        abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0,
        0.0,
        1.0
    );

    rgb = rgb * rgb * (3.0 - 2.0 * rgb);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

float pulse(float phase, float beat, float width)
{
    // Periodic Gaussian: continuous across the CPU's phase wrap at 1 -> 0.
    float distance = abs(fract(phase - beat + 0.5) - 0.5) / width;
    return exp(-0.5 * distance * distance);
}

void legacyEffect()
{
    if (fx.effectEnabled == 0u) { outColor = vec4(0.0); return; }
    if (fx.debugMode == 4u) {
        outColor = fx.viewport.w > 0.5 ? vec4(texture(sceneTexture, uv).rgb, 1.0) : vec4(0.0);
        return;
    }
    if(fx.debugMode==3u && fx.aoConfig.w==0u && fx.depthConfig.z==0u) {outColor=vec4(0);return;}
    ivec2 size = textureSize(depthTexture, 0);
    vec2 px = 1.0 / vec2(size);

    float center = getDepth(uv);

    if (fx.debugMode == 1u) { outColor = vec4(vec3(rawDepth(uv)), 1.0); return; }

    if (fx.debugMode == 5u) {
        float gradient = length(vec2(getDepth(uv+vec2(px.x,0))-center, getDepth(uv+vec2(0,px.y))-center));
        outColor = vec4(vec3(clamp(gradient*32.0,0.0,1.0)),1.0); return;
    }
    if (fx.debugMode == 6u) {
        outColor = linearDepthReady() ? vec4(vec3(linearDepth(uv)/fx.projection.y),1.0) : vec4(0.0);
        return;
    }
    if (fx.debugMode == 8u || fx.debugMode == 14u) {
        if (!positionReady()) { outColor=vec4(0.0); return; }
        vec3 position = viewPosition(uv);
        outColor = vec4(fx.debugMode == 8u ? vec3(-position.z/fx.projection.y) : .5+position/(2.0*fx.projection.y),1.0);
        return;
    }
    if (fx.debugMode == 7u) {
        outColor = !positionReady() ? vec4(0) : vec4(center>=.99999?vec3(0):viewNormal(uv)*.5+.5,1);
        return;
    }
    if (center >= 0.99999)
    {
        outColor = fx.debugMode == 2u ? vec4(1.0) : vec4(0.0);
        return;
    }

    vec2 dirs[8] = vec2[](
        vec2( 1.0,  0.0),
        vec2(-1.0,  0.0),
        vec2( 0.0,  1.0),
        vec2( 0.0, -1.0),
        vec2( 0.707,  0.707),
        vec2(-0.707,  0.707),
        vec2( 0.707, -0.707),
        vec2(-0.707, -0.707)
    );

    float ao = 0.0;
    float radii[3] = float[](fx.aoRadius * (3.0 / 16.0), fx.aoRadius * 0.5, fx.aoRadius);

    for (int r = 0; r < 3; ++r)
    {
        for (int i = 0; i < 8; ++i)
        {
            float neighbour =
                getDepth(uv + dirs[i] * px * radii[r]);

            float delta = center - neighbour;
            float scale = max(1.0 - center, 0.0002);
            float relativeDelta = delta / scale;

            ao += smoothstep(fx.aoBias, fx.aoBias + 0.038, relativeDelta);
        }
    }

    ao /= 24.0;
    ao = clamp(pow(ao, 0.65) * fx.aoStrength, 0.0, 1.0);
    legacyOcclusion = ao;
    if (fx.debugMode == 2u) { outColor = vec4(vec3(1.0 - ao), 1.0); return; }

    // Güçlü görünmesi için alpha
    float alpha = clamp(ao * fx.opacity, 0.0, fx.opacity);

    if (fx.debugMode == 0u || (fx.debugMode == 3u && fx.aoConfig.w==0u)) { outColor = vec4(0.0, 0.0, 0.0, alpha); return; }

    // Psychedelic rainbow hue:
    // depth + ekran konumu + dalgalı gradient
    float hue =
        fract(
            ((1.0 - center) * 1.2 +
            uv.x * 0.7 +
            uv.y * 0.35 +
            0.10 * sin(uv.x * 40.0) +
            0.08 * cos(uv.y * 32.0) +
            0.06 * sin((uv.x + uv.y) * 55.0)) * fx.hueFrequency
        );

    if (fx.heartbeat.y > 0.0)
    {
        // Aspect correction keeps the wave circular on a widescreen viewport.
        vec2 fromCenter = (uv - vec2(0.5)) * vec2(fx.heartbeat.z, 1.0);
        float radius = length(fromCenter) * 2.0;
        // More distant pixels see the same beat later: propagation is outward.
        float localPhase = fract(fx.heartbeat.x - radius * 0.42);
        float beat = max(pulse(localPhase, 0.10, 0.045),
                         0.70 * pulse(localPhase, 0.30, 0.060));
        float strength = clamp(fx.heartbeat.y, 0.0, 1.0);
        // Keep phase periodic even at partial strength; blending HSV phases
        // directly by strength would introduce a jump at each phase wrap.
        float radialHue = fract(hue + radius * 0.65 - fx.heartbeat.x + beat * 0.08);
        vec3 animated = hsv2rgb(vec3(radialHue, 0.90, 1.0));
        vec3 original = hsv2rgb(vec3(hue, 0.90, 1.0));
        vec3 rainbow = mix(original, animated, strength);
        rainbow *= clamp(ao * 1.8, 0.0, 1.0);
        // Modulate the existing AO mask, leaving non-AO areas transparent.
        alpha *= mix(1.0, 0.45 + 0.55 * beat, strength);
        outColor = vec4(rainbow * fx.rainbowStrength, alpha);
        return;
    }

    float sat = 0.90;
    float val = 1.00;

    vec3 rainbow = hsv2rgb(vec3(hue, sat, val));

    // AO azsa renk de biraz yumuşasın
    rainbow = mix(vec3(0.0), rainbow, clamp(ao * 1.8, 0.0, 1.0));

    outColor = vec4(rainbow * fx.rainbowStrength, alpha);
}

