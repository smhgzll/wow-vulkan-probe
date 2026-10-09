#!/usr/bin/python3
"""Replays the water composite shader on a saved debug capture (exports/water-capture-*) and writes PNGs.

  tools/water-preview.py exports/water-capture-92693-1-mode1-view0 --out /tmp/preview --time 0 6 12
  tools/water-preview.py <capture> --foam 1.5 1 1 0.5 --pulse 0.7 --reach 1 --detail 0.6 --quality 2 --repeat 50
  tools/water-preview.py <capture> --debug 6 --wave-height 2      (the Gerstner waves the capture recorded, twice as high)

Builds the params block from the capture (options/tone can be overridden), runs tools/water_preview.cpp on the GPU (built on first use into
/tmp/depth-probe-water-preview) and converts the result. Prints the GPU time of one composite pass, which is what the water costs per frame on top
of the game's own rendering (the mask/planar passes are separate).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
TONES = [("NATURAL", (0, 0, 0), 0), ("TROPICAL", (.10, .72, .75), .55), ("DEEP BLUE", (.06, .24, .60), .60),
         ("EMERALD", (.08, .58, .36), .55), ("GOLDEN", (.80, .58, .30), .45)]


def sun_toward(capture, spec):
    """Toward the sun (world) for the MODERN sun terms: explicit x,y,z, none, or the brightest direction of the capture's sky memory
    (128x64 RGBA16F, u = atan2(y, x) / 2 pi + .5, v = elevation 0..90 degrees): where the screen showed the sun or its glow."""
    if spec == "none":
        return None
    if spec != "auto":
        v = np.array([float(x) for x in spec.split(",")])
        return tuple(v / np.linalg.norm(v))
    path = capture / "sky_memory.bin"
    if not path.exists():
        return None
    sky = np.fromfile(path, np.float16).astype(np.float32).reshape(64, 128, 4)[..., :3]
    luma = np.nan_to_num(sky @ np.array([.299, .587, .114], np.float32))
    y, x = np.unravel_index(np.argmax(luma), luma.shape)
    azimuth = ((x + .5) / 128 - .5) * 2 * np.pi
    elevation = (y + .5) / 64 * np.pi / 2
    print(f"sun from the sky memory: elevation {np.degrees(elevation):.1f} azimuth {np.degrees(azimuth):.1f} (luma {luma[y, x]:.2f})")
    return (np.cos(elevation) * np.cos(azimuth), np.cos(elevation) * np.sin(azimuth), np.sin(elevation))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--out", default="/tmp/depth-probe-water-preview-out")
    ap.add_argument("--time", type=float, nargs="+", default=[0.0])
    ap.add_argument("--foam", type=float, nargs=4, default=[1, 1, 1, .5], metavar=("AMOUNT", "SIZE", "SPEED", "SOFTNESS"))
    ap.add_argument("--pulse", type=float, default=.7)
    ap.add_argument("--reach", type=float, default=1.0)
    ap.add_argument("--detail", type=float, default=.6)
    ap.add_argument("--opacity", type=float, default=1.0, help="opacity of the game's own water texture")
    ap.add_argument("--clarity", type=float, default=.6)
    ap.add_argument("--caustics", type=float, default=.8)
    ap.add_argument("--glitter", type=float, default=1.0)
    ap.add_argument("--whitecaps", type=float, default=.5)
    ap.add_argument("--blend-mode", type=float, default=0.0, help="water blend equation: 0 unknown (clarity off), 1 alpha, 2 premultiplied")
    ap.add_argument("--quality", type=int, default=1)
    ap.add_argument("--layers", type=int, default=15)
    ap.add_argument("--tone", type=int, default=None, help="index into the tone table; default: the capture's tone")
    ap.add_argument("--strength", type=float, default=None)
    ap.add_argument("--mode", type=int, default=1, help="1 = SSR (planar needs the planar passes, which the tool does not have)")
    ap.add_argument("--debug", type=int, default=0, help="debug view (water_reflections.hpp waterDebugViewNames): 6 displacement, 7 large waves")
    ap.add_argument("--style", type=int, default=1, help="1 MODERN, 0 CLASSIC (A/B)")
    ap.add_argument("--roughness", type=float, default=.06)
    ap.add_argument("--medium", type=float, default=1.0, help="medium ripple scale (MODERN)")
    ap.add_argument("--micro", type=float, default=1.0, help="micro ripple scale (MODERN)")
    ap.add_argument("--sss", type=float, default=1.0, help="subsurface light (MODERN)")
    ap.add_argument("--absorption", type=float, nargs=3, default=[.31, .055, .008], help="per yard r g b (x clarity scale)")
    ap.add_argument("--sun", default="auto", help="auto (the capture's recorded sun, else the brightest sky-memory direction), sky, none, or x,y,z "
                    "toward the sun (world)")
    ap.add_argument("--sun-color", default=None, help="r,g,b (default: the capture's recorded sun colour, else 1,.93,.8)")
    ap.add_argument("--default-waves", action="store_true", help="use the layer's default Gerstner waves (for captures without waves)")
    ap.add_argument("--wave-height", type=float, default=1.0,
                    help="x the amplitudes of the Gerstner waves the capture recorded (captures before the waves have none: flat)")
    ap.add_argument("--repeat", type=int, default=20)
    ap.add_argument("--crop", type=int, nargs=4, default=None, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--gpu", default="")
    ap.add_argument("--shaders", default=str(ROOT / "shaders"), help="directory with the composite .spv and the texture files")
    args = ap.parse_args()

    capture = Path(args.capture)
    meta = json.loads((capture / "capture.json").read_text())
    p = meta["params"]
    tool = Path("/tmp/depth-probe-water-preview")
    source = ROOT / "tools" / "water_preview.cpp"
    newest = max(source.stat().st_mtime, (ROOT / "src" / "water_surface.cpp").stat().st_mtime, (ROOT / "src" / "water_surface.hpp").stat().st_mtime)
    if not tool.exists() or tool.stat().st_mtime < newest:
        subprocess.run(["g++", "-O1", "-std=c++20", "-I" + str(ROOT / "src"), str(source), str(ROOT / "src" / "water_surface.cpp"), "-lvulkan",
                        "-o", str(tool)], check=True)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    tone = p["tone"] if args.tone is None else [*TONES[args.tone][1], TONES[args.tone][2]]
    for t in args.time:
        options = [float(args.mode), args.strength if args.strength is not None else p["options"][1], float(args.debug), t]
        draw = [int(p["draw"][0]), int(p["draw"][1]), 0, args.layers]
        blob = struct.pack("<4f4f4I4I4i4f4f4f4f4f4f", *p["viewport"], *p["depthRange"], *[int(v) for v in p["vertex"]], *[int(v) for v in p["indices"]],
                           *draw, *options, *tone, args.foam[0], args.foam[1], t * args.foam[2], args.foam[3], args.pulse, args.reach, args.detail, float(args.quality), args.opacity, args.blend_mode, 0.0, 0.0, args.clarity, args.caustics, args.glitter, args.whitecaps)
        # The Gerstner waves as the layer packed them for that frame (water_surface.hpp PackedWaves); their phases are the capture's
        # moment, so --time does not move them. --wave-height scales the amplitudes (and the sums in `surface`).
        wave_a = list(p.get("waveA", [0.0] * 20)); wave_b = list(p.get("waveB", [0.0] * 20)); surface = list(p.get("surface", [0.0] * 4))
        for i in range(5):
            wave_a[i * 4 + 3] *= args.wave_height
        surface[1] *= args.wave_height; surface[2] *= args.wave_height; surface[3] *= args.wave_height
        blob += struct.pack("<20f20f4f", *wave_a, *wave_b, *surface)
        recorded = p.get("sun", [0.0] * 4)
        if args.sun == "auto" and recorded[3] > 0.5:
            sun = tuple(recorded[:3])
        else:
            sun = sun_toward(capture, "auto" if args.sun == "sky" else args.sun)
        if args.sun_color:
            sun_color = [float(x) for x in args.sun_color.split(",")]
        else:
            sun_color = list(p["sunColor"][:3]) if "sunColor" in p and recorded[3] > 0.5 else [1.0, .93, .8]
        blob += struct.pack("<4f4f4f4f", float(args.style), args.roughness, args.medium, args.micro,
                            *(sun if sun else (0.0, 0.0, 0.0)), 2.0 if sun else 0.0, *sun_color, args.glitter,
                            *args.absorption, args.sss)
        assert len(blob) == 416, len(blob)
        params_path = out / "params.bin"
        params_path.write_bytes(blob)
        raw_path = out / f"t{t:g}.raw"
        env = dict(os.environ)
        if args.default_waves:
            env["DEPTH_PROBE_PREVIEW_WAVES"] = str(args.wave_height)
        run = subprocess.run([str(tool), args.shaders, str(capture), str(params_path), str(raw_path), str(args.repeat), args.gpu],
                             capture_output=True, text=True, env=env)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            sys.exit(run.returncode)
        image = np.fromfile(raw_path, dtype=np.uint8).reshape(1080, 1920, 4)[..., [2, 1, 0]]   # B8G8R8A8 -> RGB
        if args.crop:
            x0, y0, x1, y1 = args.crop
            image = image[y0:y1, x0:x1]
        png = out / f"t{t:g}.png"
        Image.fromarray(image).save(png)
        raw_path.unlink()
        print("wrote", png)


if __name__ == "__main__":
    main()
