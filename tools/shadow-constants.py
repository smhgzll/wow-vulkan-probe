#!/usr/bin/python3
"""Find the game's sun shadow cascade matrices in the constants of the draws that sample them (roadmap M10).

  tools/shadow-constants.py --request       # ask the running game for a frame scan with constants, wait, analyse
  tools/shadow-constants.py [DIR]           # analyse a frame-scan folder made with "uniforms" (the newest when omitted)

The layer's frame scan with the word "uniforms" in its request copies the uniform buffers of every draw that samples a
large square depth image with a comparison (WoW 3.3.5a: four 2048^2 D24S8 sun cascades) and records the fog's camera and
the game's light per frame. A light-space (orthographic) matrix has one row along the sun direction (depth) and two rows
across it (u, v). This scans every block of 3-4 consecutive vec4 registers, as rows and as columns, against the sun
direction in world space and in view space, and ranks the blocks that look like one - the cascades should appear as 4
blocks with the same axes and growing extents. Also prints the registers around the best blocks.
"""
import argparse, glob, json, os, subprocess, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))


def request():
    before = set(glob.glob(os.path.join(EXPORTS, "frame-scan-*")))
    with open(os.path.join(EXPORTS, "frame-scan-request"), "w") as f:
        f.write("2 uniforms\n")
    print("requested a 2-frame scan with the shadow-sampling constants (the layer checks every 30 presents)")
    for _ in range(120):
        time.sleep(1)
        new = [d for d in glob.glob(os.path.join(EXPORTS, "frame-scan-*")) if d not in before and os.path.exists(os.path.join(d, "DONE"))]
        if new:
            time.sleep(1)
            return new[0]
    sys.exit("no scan appeared in 2 minutes (is the game running with the layer?)")


def unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def light_like(rows, s):
    """rows: 3 or 4 vectors (xyz + w). Score how much they look like an orthographic light basis for direction s."""
    xyz = [np.asarray(r[:3], float) for r in rows[:3]]
    norms = [np.linalg.norm(x) for x in xyz]
    if min(norms) < 1e-6: return None
    dirs = [x / n for x, n in zip(xyz, norms)]
    along = [abs(float(d @ s)) for d in dirs]
    k = int(np.argmax(along))
    if along[k] < .995: return None
    others = [i for i in range(3) if i != k]
    if max(along[i] for i in others) > .03: return None
    if abs(float(dirs[others[0]] @ dirs[others[1]])) > .03: return None
    return {"depthRow": k, "scales": norms, "offsets": [float(r[3]) if len(r) > 3 else 0.0 for r in rows[:3]],
            "extent": [2.0 / norms[i] for i in others], "error": 1 - along[k] + max(along[i] for i in others)}


def analyse(d):
    j = json.load(open(glob.glob(os.path.join(d, "capture-*.json"))[0]))
    shaders = {p["id"]: p for p in j["pipelines"]}
    total = hits = 0
    found = []
    tables = {}
    for frame in j["frames"]:
        ctx = frame.get("context")
        if not ctx or not ctx.get("gameSunOk"):
            print("frame %s: no fog context / game light - run with the fog on" % frame["number"]); continue
        # The shadows may follow the game's light block or the sky disc direction (the fog sun; Shadowglen: the pixel shader's
        # c20 was exactly the sky direction in view space) - test both, in world space and in every axis order of view space.
        R, U, F = unit(ctx["right"]), unit(ctx["up"]), unit(ctx["forward"])
        spaces = []
        for label, toward in (("light", ctx["gameLightToward"]), ("sky", ctx["fogSunToward"])):
            s_world = unit(toward)
            spaces.append(("world/" + label, s_world))
            view = [s_world @ R, s_world @ U, s_world @ F]
            for order, name in (((0, 1, 2), "RUF"), ((0, 2, 1), "RFU"), ((1, 0, 2), "URF"), ((1, 2, 0), "UFR"), ((2, 0, 1), "FRU"), ((2, 1, 0), "FUR")):
                spaces.append(("view%s/%s" % (name, label), unit([view[k] for k in order])))
        for draw in frame["draws"]:
            # A draw INTO a large square depth target is a shadow pass (its own light matrices), else it samples the maps.
            dt = draw.get("target", {}).get("depth") or {}
            role = "pass" if dt.get("width", 0) >= 1024 and dt.get("width") == dt.get("height") else "recv"
            for u in draw.get("uniforms", []):
                total += 1
                if not u["file"]: continue
                data = np.fromfile(os.path.join(d, u["file"]), np.float32)
                regs = data[: len(data) // 4 * 4].reshape(-1, 4)
                hits += 1
                p = shaders.get(draw["pipeline"], {})
                key = (p.get("vs", "?")[:8], p.get("fs", "?")[:8], u["set"], u["binding"], role, dt.get("image", "")[-6:])
                tables.setdefault(key, regs)
                for space, s in spaces:
                    for i in range(len(regs) - 2):
                        block = regs[i:i + 4]
                        for layout, rows in (("rows", block), ("columns", block.T if len(block) == 4 else None)):
                            if rows is None: continue
                            r = light_like(rows, s)
                            if r: found.append((r["error"], key, i, space, layout, r, frame["number"], draw["order"]))
    print("constant buffers: %d captured of %d observed" % (hits, total))
    if not found:
        print("no light-space block found (matrices may be combined with the camera, or the light is not the game light block's)")
        return
    seen = set()
    print("\nlight-space blocks (ranked; extent = 2 / |row| = the cascade's width in yards if the map spans -1..1):")
    for err, key, i, space, layout, r, fn, order in sorted(found, key=lambda x: x[0]):
        tag = (key, i, space, layout)
        if tag in seen: continue
        seen.add(tag)
        print("  %s%s vs %s fs %s set%d/b%d  c%-3d %-14s %-7s depth row %d  extent %.1f x %.1f yd  offsets %s  error %.4f  (frame %s draw %s)"
              % (key[4], (" ->" + key[5]) if key[4] == "pass" else "", key[0], key[1], key[2], key[3], i, space, layout, r["depthRow"], r["extent"][0], r["extent"][1],
                 ", ".join("%.3f" % o for o in r["offsets"]), err, fn, order))
        if len(seen) >= 40: break
    best = sorted(found, key=lambda x: x[0])[0]
    regs = tables[best[1]]
    lo, hi = max(0, best[2] - 4), min(len(regs), best[2] + 20)
    print("\nregisters c%d..c%d of vs %s fs %s set%d/b%d (%s %s):" % (lo, hi - 1, *best[1]))
    for i in range(lo, hi): print("  c%-3d % 12.5f % 12.5f % 12.5f % 12.5f" % (i, *regs[i]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--request", action="store_true")
    o = ap.parse_args()
    d = request() if o.request else o.dir or max((x for x in glob.glob(os.path.join(EXPORTS, "frame-scan-*")) if os.path.isdir(os.path.join(x, "uniforms"))),
                                                   key=os.path.getmtime, default=None)
    if not d: sys.exit("no frame scan with uniforms; use --request")
    print("scan", d)
    analyse(d)


if __name__ == "__main__":
    main()
