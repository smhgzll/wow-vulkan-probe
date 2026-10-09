#!/usr/bin/python3
"""Scan whole WoW frames through the running layer - no keys, menus or restarts - and explain the frame (docs/draw-classes.md).

  tools/frame-scan.py --request [--frames 3]        # ask the running game for a scan, wait for it, analyse it
  tools/frame-scan.py --watch 6 --every 20          # a scan every 20 s while you play normally, then a summary of all
  tools/frame-scan.py [DIR]                         # analyse a scan folder (the newest when omitted)

The layer notices $DEPTH_PROBE_EXPORT_DIR/frame-scan-request within 30 presents and captures that many frames (every draw: state,
effective state, semantic class, target, pipeline, shaders) into frame-scan-<pid>-<n>/, finished by a DONE file. The report
(also written to report.md in the folder) is ranked conclusions, not a dump:
  1. the frame's passes in order (target, draw count, classes)
  2. the class sequence on the scene target: where sky / solid / blended / UI begin and end, and whether they interleave
  3. why draws stayed UNKNOWN, with the shaders behind each reason
  4. the alpha test of the solid world draws (specialised / fast-link / no pattern), alpha-tested shaders
  5. blended draw signatures (the data a PARTICLE rule would be built from)
  6. full-range (UI-like) draws: depth test on/off
  7. depth-only passes and who samples their depth images (WoW 3.3.5a draws 4 sun shadow cascades, 2048^2 D24S8)
The first captured frame can be partial (draws recorded before the request are not snapshotted); it is skipped.
"""
import argparse, collections, glob, json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPORTS = os.environ.get("DEPTH_PROBE_EXPORT_DIR", os.path.join(ROOT, "exports"))
SOLID = {"OPAQUE", "ALPHA_TEST", "SKY"}
BLENDED = {"TRANSPARENT", "ADDITIVE", "PARTICLE", "WATER"}
BLEND_NAMES = {0: "ZERO", 1: "ONE", 2: "SRC_COLOR", 3: "1-SRC_COLOR", 4: "DST_COLOR", 5: "1-DST_COLOR", 6: "SRC_ALPHA", 7: "1-SRC_ALPHA",
               8: "DST_ALPHA", 9: "1-DST_ALPHA", 10: "CONST", 14: "SRC_ALPHA_SAT"}
TOPOLOGY = {0: "points", 1: "lines", 2: "line-strip", 3: "tri-list", 4: "tri-strip", 5: "tri-fan"}


# ---- requesting ------------------------------------------------------------------------------------------------------
def game_pid():
    try:
        out = subprocess.check_output(["pgrep", "-f", "Z:.*Wow.exe"], text=True).split()
        return int(out[0]) if out else None
    except subprocess.CalledProcessError:
        return None


def layer_log(pid):
    try:
        env = open(f"/proc/{pid}/environ", "rb").read().split(b"\0")
        for e in env:
            if e.startswith(b"DEPTH_PROBE_LAYER_LOG="):
                return e.split(b"=", 1)[1].decode()
    except OSError:
        pass
    return f"/tmp/depth-probe-{pid}.log"


def request_scan(frames, timeout=90):
    pid = game_pid()
    if not pid:
        sys.exit("WoW is not running (no Z:...Wow.exe process). Start the game, get into the world, then run this again.")
    request = os.path.join(EXPORTS, "frame-scan-request")
    before = set(glob.glob(os.path.join(EXPORTS, f"frame-scan-{pid}-*")))
    with open(request, "w") as f:
        f.write(f"{frames}\n")
    started = time.time()
    while time.time() - started < timeout:
        time.sleep(0.5)
        if os.path.exists(request) and time.time() - started > 10:
            os.remove(request)
            sys.exit("The game did not pick up the request in 10 s: the running layer is an older build (restart the game once), "
                     "or the game is on a loading screen / minimised (no presents).")
        for d in set(glob.glob(os.path.join(EXPORTS, f"frame-scan-{pid}-*"))) - before:
            if os.path.exists(os.path.join(d, "DONE")):
                return d
    sys.exit(f"No finished scan after {timeout} s; see {layer_log(pid)} for [FRAME-SCAN] / [DISCOVERY-EXPORT-FAILED].")


# ---- loading ---------------------------------------------------------------------------------------------------------
def load(d):
    name = ""
    for _ in range(50):   # an older layer wrote DONE in place: wait for its content
        name = open(os.path.join(d, "DONE")).read().strip()
        if name:
            break
        time.sleep(.1)
    cap = json.load(open(os.path.join(d, name)))
    cap["_pipelines"] = {p["id"]: p for p in cap["pipelines"]}
    cap["_shaders"] = {s["hash"]: s for s in cap["shaders"]}
    return cap


def image_of(view):
    return view.get("image") if view else None


def target_key(d):
    t = d["target"]
    color = t["colors"][0] if t.get("colors") else None
    return (image_of(color), image_of(t.get("depth")))


def pass_key(d):
    # one render pass / dynamic rendering instance: its command buffer and ordinal there ("pass", layers from 2026-09-27 on)
    return (d.get("command"), d.get("commandGeneration"), d.get("pass"), target_key(d))


def target_name(d, scene):
    t = d["target"]
    color = t["colors"][0] if t.get("colors") else None
    depth = t.get("depth")
    where = "swapchain" if color and color.get("swapchain") else f"offscreen {color['width']}x{color['height']}" if color else "no colour"
    if depth and depth.get("image") == scene:
        return where + " + SCENE depth"
    return where + (" + other depth" if depth else "")


def cls(d):
    return d["semantic"]["class"]


def frames_of(cap):
    frames = cap["frames"]
    return frames[1:] if len(frames) > 1 else frames


def runs(seq):
    out = []
    for c in seq:
        if out and out[-1][0] == c:
            out[-1][1] += 1
        else:
            out.append([c, 1])
    return out


# ---- analysis ----------------------------------------------------------------------------------------------------------
def analyse(cap, lines):
    P = lines.append
    scene = cap.get("sceneDepthImage")
    frames = frames_of(cap)
    shaders, pipes = cap["_shaders"], cap["_pipelines"]
    draws = [d for f in frames for d in f["draws"]]
    n = max(len(frames), 1)
    P(f"frames analysed: {len(frames)} (of {len(cap['frames'])} captured), draws per frame {len(draws) / n:.0f}, scene depth image {scene}")
    if any(f.get("truncated") for f in cap["frames"]):
        P("WARNING: a frame was truncated (draw limit) - counts are lower bounds")
    if not draws:
        P("no draws captured - was the game on a loading screen?")
        return

    # 1. passes of the last frame
    last = frames[-1]["draws"]
    has_pass = any("pass" in d for d in last)
    P("\n1. PASSES (last frame, in submit order; " + ("a pass = one render pass instance)" if has_pass else "a pass = consecutive draws on the same colour+depth images)"))
    segments = []
    for i, d in enumerate(last):
        k = pass_key(d) if has_pass else target_key(d)
        if segments and segments[-1]["key"] == k:
            segments[-1]["draws"].append(d)
        else:
            segments.append({"key": k, "start": i, "draws": [d]})
    for s in segments:
        c = collections.Counter(cls(d) for d in s["draws"])
        P(f"  #{s['start']:<5} {len(s['draws']):>5} draws  {target_name(s['draws'][0], scene):<34} " + ", ".join(f"{k} {v}" for k, v in c.most_common()))

    # 2. class sequence on the scene target
    P("\n2. SCENE TARGET - CLASS SEQUENCE (last frame; runs of the same class, '|' = another target in between, '/' = a new render pass)")
    seq, scene_draws, prev_scene, prev_pass = [], [], True, None
    for d in last:
        on = d["target"].get("depth") and d["target"]["depth"].get("image") == scene
        if on:
            if not prev_scene and seq:
                seq.append("|")
            elif has_pass and prev_pass is not None and pass_key(d) != prev_pass:
                seq.append("/")
            seq.append(cls(d))
            scene_draws.append(d)
            prev_pass = pass_key(d)
        prev_scene = on
    text = " ".join(c if c in "|/" else f"{c}x{k}" for c, k in runs(seq))
    P("  " + (text if len(text) < 1500 else text[:1500] + " ..."))
    order = [c for c in seq if c not in "|/"]
    firsts, lasts = {}, {}
    for i, c in enumerate(order):
        firsts.setdefault(c, i)
        lasts[c] = i
    P("  first..last index per class: " + ", ".join(f"{c} {firsts[c]}..{lasts[c]}" for c in sorted(firsts, key=firsts.get)))
    solid_last = max((lasts[c] for c in SOLID if c in lasts), default=-1)
    blended_before = sum(1 for i, c in enumerate(order) if c in BLENDED and i < solid_last)
    first_blended = min((firsts[c] for c in BLENDED if c in firsts), default=None)
    if first_blended is None:
        P("  no blended draws on the scene target this frame")
    elif blended_before == 0:
        P(f"  CLEAN BOUNDARY: every solid draw (sky/opaque/alpha-test) comes before the first blended one (index {first_blended})")
        if has_pass and first_blended > 0:
            at_pass = pass_key(scene_draws[first_blended]) != pass_key(scene_draws[first_blended - 1])
            P("  the boundary " + ("IS a render pass boundary: the fog can go between the two passes"
                                   if at_pass else "is INSIDE a render pass: inserting there means splitting that pass"))
    else:
        P(f"  INTERLEAVED: {blended_before} blended draws come before the last solid draw (index {solid_last}); "
          f"first blended at {first_blended} - a single opaque/transparent split point does not exist as is")

    # milestone 2: the split rules of src/draw_segments.hpp on this frame. A blended cutout (ALPHA_TEST with the CUTOUT evidence,
    # milestone 4: WoW's foliage) belongs to the blended phase, like the depth-writing TRANSPARENT it used to be classified as.
    def cutout(d):
        return "CUTOUT" in d["semantic"]["evidence"]
    def blended_draw(d):
        return cls(d) in BLENDED or cutout(d)
    def split(starts):   # starts(scene draw) -> bool
        idx = next((i for i, d in enumerate(scene_draws) if starts(d)), None)
        if idx is None:
            return "none (no such draw)"
        solids_after = sum(1 for d in scene_draws[idx:] if cls(d) in ("SKY", "OPAQUE", "ALPHA_TEST") and not cutout(d))
        # blended draws before the split: those without depth write get the depth behind them (wrong fog); depth-writing
        # ones (WoW's second material layers) leave their own depth and are fogged right
        before = [d for d in scene_draws[:idx] if blended_draw(d)]
        blended_before = sum(1 for d in before if not d.get("effective", {}).get("depthWrite"))
        layers = len(before) - blended_before
        at_start = has_pass and idx > 0 and pass_key(scene_draws[idx]) != pass_key(scene_draws[idx - 1])
        where = ("at a render pass start" if at_start else "inside a render pass") if has_pass else "pass unknown (older layer)"
        return (f"index {idx}, solid draws after it {solids_after}, blended without depth write before it {blended_before}"
                + (f" (+{layers} depth-writing, fogged right)" if layers else "") + f", {where}")
    P("  split rule C (before the first depth-writing TRANSPARENT / blended cutout or WATER): "
      + split(lambda d: cls(d) == "WATER" or ((cls(d) == "TRANSPARENT" or cutout(d)) and d.get("effective", {}).get("depthWrite"))))
    P("  split rule B (before the first TRANSPARENT/WATER/blended cutout): " + split(lambda d: cls(d) in ("TRANSPARENT", "WATER") or cutout(d)))
    P("  split rule A (before the first blended draw):      " + split(blended_draw))

    # class totals
    P("\n   class totals per frame (scene target / elsewhere, confidence):")
    tot = collections.Counter((d["target"].get("depth") or {}).get("image") == scene and "scene" or "other" for d in draws)
    for where in ("scene", "other"):
        c = collections.Counter((cls(d), d["semantic"]["confidence"]) for d in draws if ((d["target"].get("depth") or {}).get("image") == scene) == (where == "scene"))
        P(f"   {where:<5} {tot[where] / n:6.0f}: " + ", ".join(f"{k[0]} {k[1].lower()} {v / n:.0f}" for k, v in c.most_common()))

    # 3. unknown
    P("\n3. UNKNOWN - WHY (per frame, with the fragment shaders behind it)")
    unk = [d for d in draws if cls(d) == "UNKNOWN"]
    by_reason = collections.defaultdict(list)
    for d in unk:
        by_reason[d["semantic"]["reason"]].append(d)
    for reason, ds in sorted(by_reason.items(), key=lambda x: -len(x[1])):
        fs = collections.Counter(pipes.get(d["pipeline"], {}).get("fs", "?") for d in ds)
        where = collections.Counter(target_name(d, scene) for d in ds).most_common(1)[0][0]
        P(f"  {len(ds) / n:6.1f}  {reason:<42} mostly on {where}; fs " + ", ".join(f"{h[:8]}x{c}" for h, c in fs.most_common(3)))
    if not unk:
        P("  none")

    # 4. alpha test
    P("\n4. ALPHA TEST OF SOLID WORLD DRAWS (depth write, no blend, world range) AND BLENDED CUTOUTS")
    solid = [d for d in draws if cls(d) in ("OPAQUE", "ALPHA_TEST") and "WORLD-RANGE" in d["semantic"]["evidence"] and "CUTOUT" not in d["semantic"]["evidence"]]
    src = collections.Counter(pipes.get(d["pipeline"], {}).get("alphaTestSource", "?") for d in solid)
    P("  source: " + ", ".join(f"{k} {100 * v / max(len(solid), 1):.0f}%" for k, v in src.most_common()))
    at = [d for d in solid if cls(d) == "ALPHA_TEST"]
    P(f"  alpha-tested: {len(at) / n:.0f} per frame of {len(solid) / n:.0f} solid; "
      + (", ".join(f"fs {h[:8]} x{c}" for h, c in collections.Counter(pipes[d['pipeline']]['fs'] for d in at).most_common(5)) or "none"))
    comp = collections.Counter(pipes.get(d["pipeline"], {}).get("alphaCompare") for d in solid if pipes.get(d["pipeline"], {}).get("alphaTest", -1) >= 0)
    if comp:
        P("  compare ops (VkCompareOp, 7 = ALWAYS = off): " + ", ".join(f"{k}: {v}" for k, v in comp.most_common()))

    # Roadmap M4: blended cutouts (alpha blend + alpha test + depth write - WoW's foliage), with their alpha reference.
    cut = [d for d in draws if "CUTOUT" in d["semantic"]["evidence"]]
    refs = collections.Counter(d["semantic"].get("alphaRef") for d in cut)
    P(f"  blended cutouts (ALPHA_TEST, alpha blend + alpha test + depth write): {len(cut) / n:.0f} per frame; "
      + (", ".join(f"fs {h[:8]} x{c / n:.0f}" for h, c in collections.Counter(pipes[d['pipeline']]['fs'] for d in cut).most_common(4)) or "none"))
    if cut:
        P("    alpha reference (of 255; D3DRS_ALPHAREF): " + ", ".join(f"{'unknown' if k is None else k}: {v / n:.0f}" for k, v in refs.most_common(6)))
    blended = [d for d in draws if d.get("effective", {}).get("blend") and "WORLD-RANGE" in d["semantic"]["evidence"]]
    bat = collections.Counter((cls(d), pipes.get(d["pipeline"], {}).get("alphaTest"), pipes.get(d["pipeline"], {}).get("alphaCompare"), d["effective"].get("depthWrite")) for d in blended)
    if bat:
        P("  blended world draws - alpha test too (class, alpha test, compare op, depth write): "
          + ", ".join(f"{k[0]} at{k[1]} op{k[2]} dw{k[3]}: {v / n:.0f}" for k, v in bat.most_common(6)))

    # 5. blended signatures
    P("\n5. BLENDED DRAW SIGNATURES (scene target, per frame; ff = no DXVK alpha-test pattern, i.e. fixed-function shader)")
    sig = collections.defaultdict(list)
    for d in draws:
        e = d.get("effective", {})
        if not e.get("blend"):
            continue
        if (d["target"].get("depth") or {}).get("image") != scene:
            continue
        p = pipes.get(d["pipeline"], {})
        fsinfo = shaders.get(p.get("fs"), {})
        key = (cls(d), e.get("blendSrc"), e.get("blendDst"), e.get("depthWrite"), e.get("topology"), p.get("fs", "?"), bool(fsinfo.get("dxvkAlphaTest")))
        sig[key].append(d)
    for key, ds in sorted(sig.items(), key=lambda x: -len(x[1]))[:15]:
        c, s, t, dw, topo, fs, prog = key
        counts = sorted(d["count"] for d in ds)
        P(f"  {len(ds) / n:6.1f}  {c:<11} {BLEND_NAMES.get(s, s)}/{BLEND_NAMES.get(t, t):<12} dw{dw} {TOPOLOGY.get(topo, topo):<9} "
          f"fs {fs[:8]} {'prog' if prog else 'ff  '} verts median {counts[len(counts) // 2]} max {counts[-1]}, instanced {sum(d['instances'] > 1 for d in ds)}")
    if not sig:
        P("  none")

    # 7 (printed last). the game's own shadow maps: depth-only passes whose depth images later draws sample with a comparison
    shadow_lines = []
    for fi, f in enumerate(frames[-1:]):
        fr = f["draws"]
        depth_only = collections.Counter()
        for d in fr:
            if d.get("effective", {}).get("colorMask") == 0 and d["target"].get("depth") and d["target"]["depth"].get("image") != scene:
                t = d["target"]["depth"]
                depth_only[(t["image"], t["width"], t["height"], t["format"])] += 1
        images = {k[0] for k in depth_only}
        users = collections.Counter()
        for d in fr:
            for r in d.get("resources", []):
                v = r.get("view") or {}
                if v.get("image") in images:
                    users[(v["image"], cls(d), pipes.get(d["pipeline"], {}).get("fs", "?")[:8], r["set"], r["binding"], bool(r.get("staticDref")))] += 1
        for (img, w, h, fmt), count in sorted(depth_only.items(), key=lambda x: -x[1]):
            u = [(k, v) for k, v in users.items() if k[0] == img]
            dref = sum(v for k, v in u if k[5])
            who = ", ".join(f"{k[1]} fs {k[2]} set{k[3]}/b{k[4]}" for k, v in sorted(u, key=lambda x: -x[1])[:3])
            shadow_lines.append(f"  depth {img[-8:]} {w}x{h} fmt {fmt}: {count} depth-only draws; sampled by {len(u)} bindings "
                                f"({dref} draws with a comparison sampler){' - ' + who if who else ' - never sampled this frame'}")

    # 6. full-range draws
    P("\n6. FULL-RANGE (0..1) DRAWS - UI CANDIDATES")
    full = [d for d in draws if (d.get("effective", {}).get("depthRange") or [0, 0])[0] == 0 and (d.get("effective", {}).get("depthRange") or [0, 0])[1] >= .999]
    c = collections.Counter((target_name(d, scene), d.get("effective", {}).get("depthTest"), cls(d)) for d in full)
    for (where, dt, k), v in c.most_common(8):
        P(f"  {v / n:6.1f}  depth test {dt}  on {where:<34} -> {k}")
    if not full:
        P("  none")

    P("\n7. DEPTH-ONLY PASSES (colour mask 0) - THE GAME'S SHADOW MAPS WHEN LATER DRAWS SAMPLE THEM WITH A COMPARISON")
    for line in shadow_lines or ["  none"]:
        P(line)


def summary_line(cap):
    scene = cap.get("sceneDepthImage")
    frames = frames_of(cap)
    draws = [d for f in frames for d in f["draws"]]
    n = max(len(frames), 1)
    c = collections.Counter(cls(d) for d in draws if (d["target"].get("depth") or {}).get("image") == scene)
    unk = sum(cls(d) == "UNKNOWN" for d in draws)
    return f"{len(draws) / n:5.0f} draws/frame, scene: " + " ".join(f"{k} {v / n:.0f}" for k, v in c.most_common()) + f" | unknown {100 * unk / max(len(draws), 1):.0f}%"


def report(d):
    cap = load(d)
    lines = [f"FRAME SCAN {d}"]
    analyse(cap, lines)
    text = "\n".join(lines)
    open(os.path.join(d, "report.md"), "w").write(text + "\n")
    print(text)
    return cap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--request", action="store_true")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--watch", type=int, default=0, help="number of scans")
    ap.add_argument("--every", type=float, default=20, help="seconds between watch scans")
    a = ap.parse_args()
    if a.watch:
        results = []
        for i in range(a.watch):
            d = request_scan(a.frames)
            cap = load(d)
            results.append((time.strftime("%H:%M:%S"), d, summary_line(cap)))
            print(f"[{i + 1}/{a.watch}] {results[-1][0]} {os.path.basename(d)}: {results[-1][2]}", flush=True)
            if i + 1 < a.watch:
                time.sleep(a.every)
        print("\nDetailed report of the last scan:")
        report(results[-1][1])
        return
    if a.request:
        d = request_scan(a.frames)
    else:
        d = a.dir or max(glob.glob(os.path.join(EXPORTS, "frame-scan-*")), key=os.path.getmtime, default=None)
        if not d:
            sys.exit("no frame-scan-* folder; use --request while the game runs")
    report(d)


if __name__ == "__main__":
    main()
