#!/usr/bin/python3
"""Find the game's point lights in its shader constants (research for the light gizmos).

  tools/light-constants.py --request        # ask the running game for a 2-frame "lights" scan, then analyse it
  tools/light-constants.py [DIR]            # analyse a frame-scan folder made with "lights" (the newest when omitted)

The layer's frame scan with the word "lights" copies the uniform buffers of every draw on the scene's depth target (12 per
pipeline) and records the camera. WoW 3.3.5a lights its models per vertex from constants: in Shadowglen's M2 vertex
shader 75b52ce1, c25 held the ambient colour, c26 the sun's, c28 minus the sun direction and c30..c33 were all zero - no
torch nearby. A point light needs a world position, a colour and a range. This looks, per vertex/pixel shader, for
registers whose xyz is a world position near the camera (not a terrain chunk's corner on the 33.33-yd grid), ranks them by whether a colour-like and a range-like register
sit next to them and whether the same register is zero in other draws of that shader (an unused light slot), then groups
the positions found in every draw into distinct lights.
"""
import argparse, collections, glob, json, os, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))
NEAR = 250.0   # a light position is within this many yards of the camera


def request():
    before = set(glob.glob(os.path.join(EXPORTS, "frame-scan-*")))
    with open(os.path.join(EXPORTS, "frame-scan-request"), "w") as f:
        f.write("2 lights\n")
    print("requested a 2-frame scan with every world draw's constants (the layer checks every 30 presents)")
    for _ in range(180):
        time.sleep(1)
        new = [d for d in glob.glob(os.path.join(EXPORTS, "frame-scan-*")) if d not in before and os.path.exists(os.path.join(d, "DONE"))]
        if new:
            time.sleep(1)
            return new[0]
    sys.exit("no scan appeared in 3 minutes (is the game running with the layer?)")


def colour_like(r):
    """Non-zero RGB in a light's range (each channel 0..4, not all equal to one: an identity row is not a colour)."""
    c = np.asarray(r[:3], float)
    return bool(np.all(c >= 0) and np.all(c <= 4) and c.max() > .02 and not np.allclose(c, 1) and not np.allclose(c, [1, 0, 0]) and
                not np.allclose(c, [0, 1, 0]) and not np.allclose(c, [0, 0, 1]))


def range_like(r):
    """Two ranges of a light (start < end, a few yards to a hundred) in x, y or in z, w."""
    for a, b in ((r[0], r[1]), (r[2], r[3]), (r[1], r[2])):
        if 0 <= a < b <= 150 and b > .5: return True
    return False


def chunk_corner(pos):
    """A terrain chunk's origin: x and y on the 33.33-yd ADT chunk grid (Shadowglen: c39 of every terrain draw)."""
    return all(abs(v / (100 / 3) - round(v / (100 / 3))) < 1e-3 for v in pos[:2])


def analyse(d):
    j = json.load(open(glob.glob(os.path.join(d, "capture-*.json"))[0]))
    pipes = {p["id"]: p for p in j["pipelines"]}
    per_slot = collections.defaultdict(lambda: {"filled": 0, "zero": 0, "colour": 0, "range": 0, "draws": 0, "samples": []})
    found = []   # (position, shader, stage register, neighbours, frame, draw)
    total = captured = 0
    for frame in j["frames"]:
        ctx = frame.get("context") or {}
        cam = np.asarray(ctx.get("camera", [0, 0, 0])[:3], float)
        if not ctx: print("frame %s: no camera context (scan made without 'lights'?)" % frame["number"])
        for draw in frame["draws"]:
            p = pipes.get(draw["pipeline"], {})
            for u in draw.get("uniforms", []):
                total += 1
                if not u["file"]: continue
                captured += 1
                data = np.fromfile(os.path.join(d, u["file"]), np.float32)
                regs = data[: len(data) // 4 * 4].reshape(-1, 4)
                shader = "%s set%d/b%d" % ((p.get("vs") if (u["set"], u["binding"]) == (2, 0) else p.get("fs") if (u["set"], u["binding"]) == (2, 3) else p.get("vs", "?"))[:8],
                                           u["set"], u["binding"])
                for i, r in enumerate(regs):
                    slot = per_slot[(shader, i)]
                    slot["draws"] += 1
                    if not np.any(r): slot["zero"] += 1; continue
                    pos = np.asarray(r[:3], float)
                    if not np.all(np.isfinite(pos)): continue   # unset registers can hold NaN / inf
                    dist = float(np.linalg.norm(pos - cam))
                    if np.linalg.norm(pos) < 50 or dist > NEAR or dist < .3 or chunk_corner(pos): continue   # not a light's world position
                    near = [regs[k] for k in range(max(0, i - 3), min(len(regs), i + 4)) if k != i]
                    has_colour, has_range = any(colour_like(x) for x in near), any(range_like(x) for x in near) or 0 < r[3] <= 150
                    slot["filled"] += 1; slot["colour"] += has_colour; slot["range"] += has_range
                    if len(slot["samples"]) < 3: slot["samples"].append((frame["number"], draw["order"], [round(float(x), 3) for x in r], dist))
                    found.append((pos, shader, i, has_colour, has_range, [regs[k] for k in range(max(0, i - 2), min(len(regs), i + 4))], dist, frame["number"], draw["order"]))
    print("scan %s: %d constant buffers captured of %d" % (d, captured, total))
    if not captured: return
    ranked = []
    for (shader, i), s in per_slot.items():
        if not s["filled"]: continue
        score = s["filled"] * (1 + s["colour"] / s["filled"] + s["range"] / s["filled"]) * (1.5 if s["zero"] else 1)
        ranked.append((score, shader, i, s))
    ranked.sort(key=lambda x: -x[0])
    print("\nregisters holding a world position within %d yd of the camera (ranked: a colour and a range beside it, and zero in other"
          " draws of the same shader = a light slot):" % NEAR)
    for score, shader, i, s in ranked[:25]:
        smp = s["samples"][0]
        print("  %-24s c%-3d filled %3d / zero %3d of %3d draws | colour beside %3d, range beside %3d | e.g. %s (%.1f yd away)"
              % (shader, i, s["filled"], s["zero"], s["draws"], s["colour"], s["range"], smp[2], smp[3]))
    # Distinct positions (1 yd apart) seen in any slot with a colour or a range beside it.
    lights = []
    for pos, shader, i, hc, hr, around, dist, fn, order in found:
        if not (hc or hr): continue
        for l in lights:
            if np.linalg.norm(l["pos"] - pos) < 1.0:
                l["refs"] += 1; l["shaders"].add("%s c%d" % (shader, i)); break
        else:
            lights.append({"pos": pos, "refs": 1, "shaders": {"%s c%d" % (shader, i)}, "around": around, "dist": dist})
    lights.sort(key=lambda l: -l["refs"])
    print("\n%d distinct positions with a colour or range beside them (most referenced first):" % len(lights))
    for l in lights[:20]:
        print("  (%.1f, %.1f, %.1f) %.1f yd from the camera, in %d draws via %s" % (*l["pos"], l["dist"], l["refs"], ", ".join(sorted(l["shaders"])[:3])))
        for r in l["around"]: print("      % 11.4f % 11.4f % 11.4f % 11.4f" % tuple(r))
    if not lights: print("  none - no point light near the camera in this scan, or they are not in world space (try a lit place: an inn, torches at night)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--request", action="store_true")
    o = ap.parse_args()
    d = request() if o.request else o.dir or max((x for x in glob.glob(os.path.join(EXPORTS, "frame-scan-*")) if os.path.isdir(os.path.join(x, "uniforms"))),
                                                 key=os.path.getmtime, default=None)
    if not d: sys.exit("no frame scan with constants; use --request while the game runs")
    analyse(d)


if __name__ == "__main__":
    main()
