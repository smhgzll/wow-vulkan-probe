#!/usr/bin/python3
"""Re-render a fog capture offline with other settings ("what if the density / sun / sky share were ...").

  tools/fog-whatif.py DIR --check                       # reproduce the game's colour_after from the captured volume
  tools/fog-whatif.py DIR --variant "name:key=value,..." [--variant ...] [--out file.png]

Keys: density, falloff, base (world z of the medium base), sun (intensity, colour kept), g, sky (share of the ambient from
above), ambient (scale), albedo, opensun (sun strength where the sky is open, x), weather (the sun factor the weather leaves, 1 = clear sky), skyfog (share of the fog applied over sky pixels, 1 = all), maxopacity (cap on the fog's opacity 1 - T, 1 = none), lightlimit (ceiling of the fog's light, 0 = none), exposure (x the result), g2 + lobe (a second phase lobe, its weight), gamecolor=1 (the sun colour = the game's glow colour x sun; gameSpecular in the params), color=r/g/b (x sun). The capture's density noise is kept (medium / height profile), the sun and sky visibility
come from the capture's SUN VISIBILITY frame (per froxel, lightly blurred as the temporal accumulation would), and the
integration is fog_integrate.comp's (Beer-Lambert slabs, texel k = end of slice k), the composite fog_composite.frag's.
The game frame before the fog is the background. Output: one image per variant side by side (+ the game's own result).
"""
import argparse, json, os
import numpy as np
from PIL import Image


def half(path, shape):
    return np.fromfile(path, np.uint16).view(np.float16).astype(np.float32).reshape(shape)


def load(d):
    P = json.load(open(os.path.join(d, "final_params.json")))
    sec = {s["name"]: s for lab in ("final", "visibility") if os.path.exists(os.path.join(d, lab + "_sections.json"))
           for s in json.load(open(os.path.join(d, lab + "_sections.json")))}
    gx, gy, gz = P["gridSize"]
    def vol(n):
        if sec.get(n, {}).get("format") == "rgba8":
            return (np.fromfile(os.path.join(d, n + ".bin"), np.uint8).astype(np.float32) / 255).reshape(gz, gy, gx, 4)
        return half(os.path.join(d, n + ".bin"), (gz, gy, gx, 4))
    c = sec["color_before"]
    raw = np.fromfile(os.path.join(d, "color_before.bin"), np.uint8).reshape(c["height"], c["width"], 4)
    fmt = int(c["format"].split("-")[-1])
    before = (raw[..., [2, 1, 0]] if fmt in (44, 50) else raw[..., :3]).astype(np.float32) / 255
    after = None
    if "color_after" in sec:
        raw = np.fromfile(os.path.join(d, "color_after.bin"), np.uint8).reshape(c["height"], c["width"], 4)
        after = (raw[..., [2, 1, 0]] if fmt in (44, 50) else raw[..., :3]).astype(np.float32) / 255
    depth = np.abs(np.fromfile(os.path.join(d, "view_depth.bin"), np.float32).reshape(c["height"], c["width"]))   # (water surfaces are stored negative)
    return P, vol, before, after, depth


def froxel_geometry(P):
    gx, gy, gz = P["gridSize"]
    C, lnr = P["dist"][0], P["dist"][1]
    jx, jy, jz = P["jitter"][:3]
    k, j, i = np.meshgrid(np.arange(gz), np.arange(gy), np.arange(gx), indexing="ij")
    u, v, t = (i + jx) / gx, (j + jy) / gy, (k + jz) / gz
    z = C * (np.exp(t * lnr) - 1)
    rx, ry = (u * 2 - 1) / P["proj"][0], (v * 2 - 1) / P["proj"][1]
    R, U, F = (np.array(P[n][:3]) for n in ("right", "up", "forward"))
    off = (rx * z)[..., None] * R + (ry * z)[..., None] * U + z[..., None] * F
    ray = rx[..., None] * R + ry[..., None] * U + F
    ray /= np.linalg.norm(ray, axis=-1, keepdims=True)
    return P["camera"][2] + off[..., 2], ray


def extinction(z, density, falloff, base, cap):
    return np.clip(density * np.exp(np.minimum(-(z - base) * falloff, 80)), 0, cap)


def blur(v):
    for ax in range(3):
        v = (np.roll(v, 1, ax) + 2 * v + np.roll(v, -1, ax)) / 4
    return v


def integrate(P, light):
    gx, gy, gz = P["gridSize"]
    C, lnr = P["dist"][0], P["dist"][1]
    u, v = (np.arange(gx) + .5) / gx, (np.arange(gy) + .5) / gy
    scale = np.sqrt(1 + ((u[None] * 2 - 1) / P["proj"][0]) ** 2 + ((v[:, None] * 2 - 1) / P["proj"][1]) ** 2)
    out = np.zeros_like(light)
    acc = np.zeros(light.shape[1:3] + (3,), np.float32); T = np.ones(light.shape[1:3], np.float32)
    start = 0.0
    for k in range(gz):
        end = C * (np.exp((k + 1) / gz * lnr) - 1)
        S, sig = light[k, ..., :3], light[k, ..., 3]
        ds = (end - start) * scale
        x = sig * ds
        weight = np.where(x > 1e-4, (1 - np.exp(-x)) / np.maximum(sig, 1e-12), ds)
        acc = acc + T[..., None] * S * weight[..., None]
        T = T * np.exp(-x)
        out[k, ..., :3], out[k, ..., 3] = acc, T
        start = end
    return out


def composite(P, integ, before, depth, skyfog=None, maxopacity=None, lightlimit=None, exposure=None):
    H, W = depth.shape
    gx, gy, gz = P["gridSize"]
    C, lnr, maxd = P["dist"][0], P["dist"][1], P["grid"][3]
    vp = P["viewport"]
    py, px = np.mgrid[0:H, 0:W]
    uvx, uvy = (px + .5 - vp[0]) / vp[2], (py + .5 - vp[1]) / vp[3]
    z = np.where(depth > 1e5, maxd, np.minimum(depth, maxd))
    t = np.clip(np.log1p(np.maximum(z, 0) / C) / lnr, 0, 1)
    fx, fy = np.clip(uvx * gx - .5, 0, gx - 1), np.clip(uvy * gy - .5, 0, gy - 1)
    kf = t * gz - 1
    x0, y0 = np.floor(fx).astype(int), np.floor(fy).astype(int)
    x1, y1 = np.minimum(x0 + 1, gx - 1), np.minimum(y0 + 1, gy - 1)
    wx, wy = (fx - x0)[..., None], (fy - y0)[..., None]
    k0 = np.clip(np.floor(kf).astype(int), 0, gz - 1); k1 = np.minimum(k0 + 1, gz - 1); wk = np.clip(kf - np.floor(kf), 0, 1)[..., None]
    def bil(k):
        return ((integ[k, y0, x0] * (1 - wx) + integ[k, y0, x1] * wx) * (1 - wy) + (integ[k, y1, x0] * (1 - wx) + integ[k, y1, x1] * wx) * wy)
    s = bil(k0) * (1 - wk) + bil(k1) * wk
    first = bil(np.zeros_like(k0))
    near = np.clip(t * gz, 0, 1)[..., None]
    none = np.concatenate([np.zeros_like(first[..., :3]), np.ones_like(first[..., 3:])], -1)
    s = np.where((kf < 0)[..., None], none * (1 - near) + first * near, s)
    # maxopacity: the fog never hides more than this share of what is behind it (1 - T capped); its light is scaled with the
    # opacity so the colour stays (Unreal's Fog Max Opacity)
    m = P.get("maxOpacity", 1.0) if maxopacity is None else maxopacity
    if m < 1:
        op = 1 - s[..., 3:]
        scale = np.where(op > m, m / np.maximum(op, 1e-6), 1.0)
        s = np.concatenate([s[..., :3] * scale, 1 - op * scale], -1)
    # lightlimit: the brightest channel of the fog's light bent toward this ceiling above half of it (fog::soft_limit)
    c = P.get("lightLimit", 0.0) if lightlimit is None else lightlimit
    if c > 0:
        peak = s[..., :3].max(-1, keepdims=True)
        knee, room = .5 * c, .5 * c
        bent = np.where(peak > knee, knee + room * (1 - np.exp(-(peak - knee) / room)), peak)
        s = np.concatenate([s[..., :3] * np.where(peak > 0, bent / np.maximum(peak, 1e-9), 1.0), s[..., 3:]], -1)
    k = P.get("skyFog", 1.0) if skyfog is None else skyfog   # sky pixels take this share of the fog (sky_fog)
    sky = (depth > 1e5)[..., None]
    s = np.where(sky, np.concatenate([s[..., :3] * k, 1 - k * (1 - s[..., 3:])], -1), s)
    e = (P.get("exposure", 1.0) if P.get("autoExposure") else 1.0) if exposure is None else exposure   # auto exposure (x the result)
    return np.clip((before * s[..., 3:] + s[..., :3]) * e, 0, 1)


def render(P, vol, before, depth, over):
    zw, ray = froxel_geometry(P)
    med = vol("medium")
    old = extinction(zw, P["medium"][0], P["medium"][1], P["medium"][2], P["medium"][3])
    noise = np.where(old > 1e-7, med[..., 3] / np.maximum(old, 1e-12), 1.0)
    density, falloff, base = over.get("density", P["medium"][0]), over.get("falloff", P["medium"][1]), over.get("base", P["medium"][2])
    sig = extinction(zw, density, falloff, base, P["medium"][3]) * noise
    albedo = np.array(P["albedo"][:3]) * (over["albedo"] / P["albedo"][0] if "albedo" in over else 1)
    g = over.get("g", P["albedo"][3])
    # sunColor = sunColorClear x sunWeatherScale (rain / snow / sandstorm hide the sun); weather= replaces that factor
    clear = np.array(P.get("sunColorClear", P["sunColor"][:3]))
    sunc = clear * (over["sun"] / clear[0] if "sun" in over and clear[0] > 0 else 1)
    if "color" in over:          # an explicit sun colour (r/g/b), times the sun intensity
        sunc = np.array(over["color"]) * over.get("sun", 1.0)
    elif over.get("gamecolor"):  # the game's glow colour (light block +0xA0, in the params as gameSpecular) x intensity
        sunc = np.array(P.get("gameSpecular", [1, 1, 1])) * over.get("sun", clear[0])
    sunc = sunc * over.get("weather", P.get("sunWeatherScale", 1.0))
    amb = np.array(P["ambient"][:3]) * over.get("ambient", 1.0)
    share = over.get("sky", P["memoryC"][1])
    vis = blur(vol("visibility"))
    sunVis, skyVis = vis[..., 0], vis[..., 2]
    sd = np.array(P["sunDir"][:3])
    cos = ray @ sd
    def hg(gv):
        return (1 - gv * gv) / (4 * np.pi * (1 + gv * gv - 2 * gv * cos) ** 1.5)
    # g2 / lobe: a second Henyey-Greenstein lobe (e.g. a strong forward one) mixed in with weight `lobe`
    phase = (1 - over.get("lobe", P.get("phaseLobe", 0.0))) * hg(g) + over.get("lobe", P.get("phaseLobe", 0.0)) * hg(over.get("g2", P.get("phaseG2", 0.8)))
    # opensun: the sun's strength where the sky is open (height memory), relative to under cover (1 = no difference)
    captured = P["memoryC"][3] if int(P["memoryB"][3]) & 1 else 1.0   # the capture's own open_sun (off = 1)
    openScale = 1 - (1 - over.get("opensun", captured)) * skyVis
    L = amb[None, None, None] * (1 - share + share * skyVis)[..., None] + (sunc[None, None, None] * (phase * sunVis * openScale * (P["sunDir"][3] > 0))[..., None])
    light = np.concatenate([albedo * sig[..., None] * L, sig[..., None]], -1).astype(np.float32)
    return composite(P, integrate(P, light), before, depth, over.get("skyfog"), over.get("maxopacity"), over.get("lightlimit"), over.get("exposure"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--variant", action="append", default=[])
    ap.add_argument("--out")
    a = ap.parse_args()
    P, vol, before, after, depth = load(a.dir)
    if a.check:
        mine = composite(P, vol("integrated"), before, depth)
        err = np.abs(mine - after) * 255
        print(f"captured integrated volume re-composited vs the game's colour_after: mean |diff| {err.mean():.2f}/255, p99 {np.percentile(err, 99):.1f}, max {err.max():.0f}")
        same = render(P, vol, before, depth, {})
        err2 = np.abs(same - after) * 255
        print(f"re-lit from the visibility volume with the capture's own settings vs colour_after: mean |diff| {err2.mean():.2f}/255, p99 {np.percentile(err2, 99):.1f}")
        return
    images, names = [before, after], ["game, no fog", "game, fog as captured"]
    for spec in a.variant:
        name, _, kv = spec.partition(":")
        over = {k: ([float(c) for c in v.split("/")] if "/" in v else float(v)) for k, v in (x.split("=") for x in kv.split(",") if x)}
        images.append(render(P, vol, before, depth, over)); names.append(name)
        print(f"{name}: {over} -> mean change vs no fog {np.abs(images[-1] - before).mean() * 255:.1f}/255")
    small = [np.asarray(Image.fromarray((np.clip(im, 0, 1) * 255).astype(np.uint8)).resize((640, 360))) for im in images]
    while len(small) % 2: small.append(np.zeros_like(small[0]))
    sheet = np.vstack([np.hstack(small[i:i + 2]) for i in range(0, len(small), 2)])
    out = a.out or os.path.join(a.dir, "whatif.png")
    Image.fromarray(sheet).save(out)
    print("wrote", out, "order:", ", ".join(names))


if __name__ == "__main__":
    main()
