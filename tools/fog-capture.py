#!/usr/bin/python3
"""Look at a fog capture ($DEPTH_PROBE_EXPORT_DIR/fog-capture-<pid>-<n>/, docs/fog.md "Fog capture").

  tools/fog-capture.py [DIR]          # newest capture when DIR is omitted; writes sheet.png into DIR and prints a summary
  tools/fog-capture.py --request      # ask the running game for a capture (creates the request file the layer polls)

sheet.png: game frame before the fog | with the fog | the fog's contribution x4 | SUN/SKY VISIBILITY view.
The summary: the lighting terms (sun vs ambient, phase toward the view centre and toward the sun), density at the camera,
visibility of the froxels by distance (sun and sky), how much of the height memory is filled, where the sun disc is.
"""
import glob, json, os, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))


def half(a):
    return np.frombuffer(a.tobytes(), np.float16).astype(np.float32)


def load_sections(d):
    out = {}
    for label in ("final", "visibility"):
        p = os.path.join(d, label + "_sections.json")
        if os.path.exists(p):
            for s in json.load(open(p)):
                out[s["name"]] = s
    return out


def color(d, s):
    raw = np.fromfile(os.path.join(d, s["name"] + ".bin"), np.uint8).reshape(s["height"], s["width"], -1)
    fmt = int(s["format"].split("-")[-1]) if s["format"].startswith("vk-format-") else 37
    if fmt in (44, 50):  # B8G8R8A8
        return raw[..., [2, 1, 0]]
    if fmt in (37, 43):  # R8G8B8A8
        return raw[..., :3]
    raise SystemExit(f"colour format {fmt} not handled")


def volume(d, s):
    if s["format"] == "rgba8":
        v = np.fromfile(os.path.join(d, s["name"] + ".bin"), np.uint8).astype(np.float32) / 255
    else:
        v = half(np.fromfile(os.path.join(d, s["name"] + ".bin"), np.uint16))
    return v.reshape(s["depth"], s["height"], s["width"], 4)


def phase(cos, g):
    gg = g * g
    return (1 - gg) / (4 * np.pi * (1 + gg - 2 * g * cos) ** 1.5)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--request":
        open(os.path.join(EXPORTS, "fog-capture-request"), "w").write("1")
        print("requested: the layer picks it up within 30 frames (fog must be on)")
        return
    d = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(EXPORTS, "fog-capture-*")), key=os.path.getmtime)
    sec = load_sections(d)
    P = json.load(open(os.path.join(d, "final_params.json")))
    print("capture", d)
    tiles = []
    if "color_before" in sec and "color_after" in sec:
        before, after = color(d, sec["color_before"]).astype(np.float32), color(d, sec["color_after"]).astype(np.float32)
        contribution = np.clip((after - before) * 4 + 128, 0, 255)
        tiles += [before, after, contribution]
        diff = after - before
        print(f"fog changed the frame by mean {np.abs(diff).mean():.1f}/255 (max {np.abs(diff).max():.0f}); mean brightness {before.mean():.1f} -> {after.mean():.1f}")
    if "visibility_view" in sec:
        tiles.append(color(d, sec["visibility_view"]).astype(np.float32))
    if tiles:
        h, w = tiles[0].shape[:2]
        small = [np.asarray(Image.fromarray(t.astype(np.uint8)).resize((w // 2, h // 2))) for t in tiles]
        while len(small) % 2:
            small.append(np.zeros_like(small[0]))
        rows = [np.hstack(small[i:i + 2]) for i in range(0, len(small), 2)]
        Image.fromarray(np.vstack(rows)).save(os.path.join(d, "sheet.png"))
        print("wrote", os.path.join(d, "sheet.png"), "(before | after / contribution x4 | visibility view)")

    # lighting terms
    amb = np.array(P["ambient"][:3]); sun = np.array(P["sunColor"][:3]); g = P["albedo"][3]; albedo = np.array(P["albedo"][:3])
    sunDir = np.array(P["sunDir"][:3]); fwd = np.array(P["forward"][:3])
    cosView = float(fwd @ sunDir)
    print(f"sun on: {P['sunDir'][3] > 0}  sun colour {np.round(sun, 2)}  ambient {np.round(amb, 3)}  g {g:.2f}  albedo {np.round(albedo, 2)}  sky share {P['memoryC'][1]:.2f}")
    print(f"phase toward the view centre (cos {cosView:+.2f}) {phase(cosView, g):.3f}, looking into the sun {phase(1.0, g):.3f}, sideways {phase(0.0, g):.3f}, isotropic {1 / (4 * np.pi):.3f}")
    print(f"sun term / ambient term: view centre {(sun * phase(cosView, g)).mean() / max(amb.mean(), 1e-6):.2f}, sideways {(sun * phase(0.0, g)).mean() / max(amb.mean(), 1e-6):.2f}  (shafts need the sun term well above the ambient)")
    base, fall, ext = P["medium"][2], P["medium"][1], P["medium"][0]
    camz = P["camera"][2]
    print(f"density at the camera {min(ext * np.exp(-(camz - base) * fall), P['medium'][3]):.4f}/yd (base z {base:.1f}, camera z {camz:.1f}, falloff {fall})")
    # the sun disc on screen
    R, U, F = np.array(P["right"][:3]), np.array(P["up"][:3]), fwd
    v = np.array([sunDir @ R, sunDir @ U, sunDir @ F])
    vp = P["viewport"]
    if v[2] > 0:
        ndc = v[:2] * [P["proj"][0], P["proj"][1]] / v[2]
        print(f"sun direction on screen: pixel ({vp[0] + (ndc[0] * .5 + .5) * vp[2]:.0f}, {vp[1] + (ndc[1] * .5 + .5) * vp[3]:.0f})")
    else:
        print("sun direction is behind the camera")
    print(f"shadows: {P['shadowState']}, march {P['shadowForward'][3]:.0f} yd, game light ok {P['gameSunOk']}, sky disc ok {P['gameSkyDiscOk']}")

    # visibility by distance (froxel slices)
    if "visibility" in sec:
        vis = volume(d, sec["visibility"])
        Z = vis.shape[0]
        C, lnr = P["dist"][0], P["dist"][1]
        print("froxel visibility by distance (share of froxels):  depth   sun lit   sky open")
        for lo, hi in ((0, 10), (10, 30), (30, 80), (80, 200), (200, 1e9)):
            ks = [k for k in range(Z) if lo <= C * (np.exp((k + .5) / Z * lnr) - 1) < hi]
            if ks:
                sl = vis[ks]
                print(f"   {lo:>4.0f}-{hi if hi < 1e8 else P['grid'][3]:<5.0f} yd   {sl[..., 0].mean():6.2f}   {sl[..., 2].mean():6.2f}")
    if "sun_map" in sec:
        sm = np.fromfile(os.path.join(d, "sun_map.bin"), np.uint32)
        print(f"sun map: {(sm != 0).mean() * 100:.1f}% of cells (across the sun) have a surface seen")
    if "height_memory" in sec:
        hm = np.fromfile(os.path.join(d, "height_memory.bin"), np.uint32)
        filled = hm != 0
        u = hm[filled]
        z = np.where(u & 0x80000000, u & 0x7fffffff, ~u).astype(np.uint32).view(np.float32)
        print(f"height memory: {filled.mean() * 100:.1f}% of cells seen; heights relative to the camera: "
              f"p10 {np.percentile(z - camz, 10):.1f}, median {np.median(z - camz):.1f}, p90 {np.percentile(z - camz, 90):.1f} yd"
              if filled.any() else "height memory: empty")


if __name__ == "__main__":
    main()
