#!/usr/bin/python3
"""Check the game's sun cascades against the scene (roadmap M10): does the layer read its shadows where the game draws them?

  tools/cascade-check.py [DIR]      # a fog capture made with game_shadows=1 (the newest when omitted)

The capture holds the four cascade maps as the world sampled them (game_cascade_K.bin, 24-bit depth), this frame's light
matrices (cascadeRowsThisFrame, finest first; rows applied to the view-space position give x, y in [-1, 1] and the depth
reference z), the camera, the scene's view depth and the game's own picture (colour_before). Every scene pixel goes through
the finest cascade that covers it; it is in shadow when the map holds something nearer to the light than its reference.
WoW draws only its casters into the maps (models, buildings - no terrain: most texels stay at depth 1), so the test is the
picture: with the right texel mapping the pixels put in shadow are clearly darker in the game's own frame (it shadowed them);
with a wrong one they are about as bright as the rest. Ranks the four axis choices by that brightness ratio.

WoW's pixel shaders, run on a frame scan's constants (tools/spirv-eval.py, 2026-09-28), sample cascade k at
(.5 + .5x, .5 + .5y) with reference z - the layer's game_shadow_flip -1.
"""
import glob, json, os, sys
import importlib.util
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(EXPORTS, "fog-capture-*")), key=os.path.getmtime)
    P = json.load(open(os.path.join(d, "final_params.json")))
    sec = {s["name"]: s for s in json.load(open(os.path.join(d, "final_sections.json")))}
    print("capture", d, "| cascades found", P.get("gameCascadesFound"), P.get("gameCascadesProblem", ""), "| reduced this frame", P.get("cascadesReducedThisFrame"))
    maps = []
    for k in range(4):
        s = sec.get("game_cascade_%d" % k)
        if not s: sys.exit("no game_cascade_%d section (fog capture with game_shadows=1 while the cascades were found)" % k)
        raw = np.fromfile(os.path.join(d, "game_cascade_%d.bin" % k), np.uint32 if s["format"] != "d16" else np.uint16)
        if s["format"] == "d24-x8": m = (raw & 0xFFFFFF).astype(np.float64) / 0xFFFFFF
        elif s["format"] == "d16": m = raw.astype(np.float64) / 0xFFFF
        else: m = raw.view(np.float32).astype(np.float64)
        maps.append(m.reshape(s["height"], s["width"]))
    rows = np.array(P["cascadeRowsThisFrame"], np.float64).reshape(4, 3, 4)
    print("matrix widths (yd):", " / ".join("%.0f" % (2 / np.linalg.norm(rows[k, 0, :3])) for k in range(4)),
          "| map bindings", P.get("gameCascadeBindings"), "| texels holding a caster: " +
          " / ".join("%.1f%%" % (100 * np.mean(m < .9999)) for m in maps))
    spec = importlib.util.spec_from_file_location("fc", os.path.join(ROOT, "tools", "fog-capture.py"))
    fc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fc)
    colour = fc.color(d, fc.load_sections(d)["color_before"]).astype(np.float64).mean(-1)
    vs = sec["view_depth"]
    depth = np.abs(np.fromfile(os.path.join(d, "view_depth.bin"), np.float32).reshape(vs["height"], vs["width"]))   # (water surfaces are stored negative)
    vp, proj = P["viewport"], P["proj"]
    h, w = depth.shape
    ys, xs = np.mgrid[0:h:3, 0:w:3]
    z = depth[ys, xs]
    ok = (z > .3) & (z < 1e5)
    xs, ys, z = xs[ok], ys[ok], z[ok]
    ndcx = ((xs + .5) - vp[0]) / vp[2] * 2 - 1
    ndcy = ((ys + .5) - vp[1]) / vp[3] * 2 - 1
    v = np.stack([ndcx / proj[0] * z, ndcy / proj[1] * z, z, np.ones_like(z)], -1)
    lum = colour[ys, xs]
    print("scene pixels sampled:", len(z))
    layer = P.get("gameShadowFlip", -1)
    results = []
    for us in (1, -1):
        for vsign in (1, -1):
            shadow = np.zeros(len(z), bool)
            done = np.zeros(len(z), bool)
            for k in range(4):
                c = v @ rows[k].T
                inside = ~done & (np.abs(c[:, 0]) < .98) & (np.abs(c[:, 1]) < .98)
                m = maps[k]
                tx = np.clip(((.5 + .5 * us * c[inside, 0]) * m.shape[1]).astype(int), 0, m.shape[1] - 1)
                ty = np.clip(((.5 + .5 * vsign * c[inside, 1]) * m.shape[0]).astype(int), 0, m.shape[0] - 1)
                shadow[np.where(inside)[0]] = c[inside, 2] > m[ty, tx] + 2e-5
                done |= inside
            if shadow.any() and (~shadow).any():
                ratio = lum[shadow].mean() / max(lum[~shadow].mean(), 1e-6)
                results.append((ratio, us, vsign, shadow.mean(), lum[shadow].mean(), lum[~shadow].mean()))
    results.sort()
    print("\ntexel mapping          in shadow   brightness shadowed / lit   (ranked: darkest shadows first)")
    for ratio, us, vsign, share, sh, lit in results:
        mark = "   <- the layer's (game_shadow_flip %+d)" % layer if us == 1 and vsign == -layer else ""
        print("  u = .5%+.1fx v = .5%+.1fy   %5.1f%%     %5.1f / %5.1f = %.2f%s" % (.5 * us, .5 * vsign, 100 * share, sh, lit, ratio, mark))
    if results and results[0][1] == 1 and results[0][2] == -layer:
        print("\nthe layer's mapping puts the darkest pixels in shadow: it reads the game's shadows where the game draws them")


if __name__ == "__main__":
    main()
