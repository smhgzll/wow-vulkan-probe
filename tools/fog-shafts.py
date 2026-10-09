#!/usr/bin/python3
"""How much of the fog is light shafts (roadmap M5): measured on fog captures, for the captured settings and variants.

  tools/fog-shafts.py DIR [DIR ...] [--variant "name:key=value,..."] ...

For every capture and variant (the keys of tools/fog-whatif.py, plus density_scale / sun_scale = x the captured value) the
fog is re-rendered twice from the capture: with its sun visibility as captured (lightly blurred, as the temporal
accumulation would) and with that visibility blurred flat (25 texels across). Reported:
  change  - the mean |fog - no fog| per pixel (/255): everything the fog does (veil, colour, shafts)
  shafts  - the mean |captured visibility - flat visibility| (/255): the part that comes from light and shadow structure
  share   - shafts / change: how much of the fog reads as shafts rather than as an even veil
  p99     - the 99th percentile of the result's brightness: highlights (blow-out risk before WoW's glow)
Findings 2026-09-27 (4 captures): the phase function (g, a second forward lobe) hardly moves the share; lower density with a
stronger sun does (x0.6 / x1.6: shafts +20-25% in the forest, change +3-9%, p99 up); open ground (Booty Bay) stays ~0.05.
"""
import argparse, importlib.util, os, sys
import numpy as np
from scipy.ndimage import uniform_filter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("whatif", os.path.join(ROOT, "tools", "fog-whatif.py"))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)
light_blur = w.blur


def flat_blur(v):
    out = v.copy()
    for c in range(v.shape[-1]): out[..., c] = uniform_filter(v[..., c], size=(3, 25, 25))
    return out


def metrics(d, over):
    P, vol, before, after, depth = w.load(d)
    over = dict(over)
    if "density_scale" in over: over["density"] = P["medium"][0] * over.pop("density_scale")
    if "sun_scale" in over: over["sun"] = P.get("sunColorClear", P["sunColor"])[0] * over.pop("sun_scale")
    try:
        w.blur = light_blur
        img = w.render(P, vol, before, depth, over)
        w.blur = flat_blur
        flat = w.render(P, vol, before, depth, over)
    finally:
        w.blur = light_blur
    change = np.abs(img - before).mean() * 255
    shafts = np.abs(img - flat).mean() * 255
    return change, shafts, np.percentile(img.mean(-1), 99) * 255


def parse_variant(text):
    name, _, body = text.partition(":")
    over = {}
    for kv in filter(None, body.split(",")):
        k, _, v = kv.partition("=")
        over[k.strip()] = float(v)
    return name, over


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--variant", action="append", default=[])
    o = ap.parse_args()
    variants = [("captured", {})] + [parse_variant(v) for v in o.variant]
    for d in o.dirs:
        print(d)
        for name, over in variants:
            change, shafts, p99 = metrics(d, over)
            print("  %-24s change %5.1f  shafts %5.2f  share %.2f  p99 %5.1f" % (name, change, shafts, shafts / max(change, 1e-3), p99))


if __name__ == "__main__":
    main()
