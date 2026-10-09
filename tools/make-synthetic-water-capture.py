#!/usr/bin/python3
"""Writes a synthetic water capture (same files as exports/water-capture-*) for tools/water-preview.py.

A real capture only shows the views the game happened to be in. This one lets water be judged from any camera: a sloping beach with a
flat sea at world z = 0 (WoW's convention: z up), viewed from a chosen height and pitch, with the sand shading and the game's own
(flat) water color faked well enough for foam, ripples and reflections to be evaluated.

  tools/make-synthetic-water-capture.py /tmp/synthetic-topdown --height 6 --pitch 62
  tools/water-preview.py /tmp/synthetic-topdown --time 0 5 10

--pitch is the angle below the horizon (90 = straight down); --height is the camera height above the water in yards.
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 1920, 1080
RANGE = (0.0, 0.94)
NEAR, FAR = 0.2, 6000.0


def smooth_noise(rng, size, out_w, out_h):
    small = rng.random((size, size)).astype(np.float32)
    return np.asarray(Image.fromarray(small).resize((out_w, out_h), Image.BICUBIC), dtype=np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--height", type=float, default=6.0)
    ap.add_argument("--pitch", type=float, default=62.0)
    ap.add_argument("--fov", type=float, default=60.0, help="vertical field of view, degrees")
    ap.add_argument("--slope", type=float, default=0.30, help="beach slope (height per yard)")
    ap.add_argument("--far", type=float, default=0.0, help="instead of a shore beside the camera, put the waterline this many yards ahead (view from afar)")
    ap.add_argument("--sun-elevation", type=float, default=14.0, help="degrees above the horizon, straight ahead of the camera")
    ap.add_argument("--sea-depth", type=float, default=3.0, help="water depth at the far side of the picture")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(7)

    # Camera at world (0,0,height), looking toward -x (the sea), pitched down.
    pitch = np.radians(args.pitch)
    forward = np.array([-np.cos(pitch), 0.0, -np.sin(pitch)])
    right = np.array([0.0, 1.0, 0.0])      # +y to the right on screen when looking toward -x with z up
    up = np.cross(right, forward)          # completes the frame; up.z > 0
    if up[2] < 0:
        up = -up
    camera = np.array([0.0, 0.0, args.height])
    # View space (+Z forward, +X right, +Y up): view = [right.p, up.p, forward.p] with p relative to the camera.
    basis = np.stack([right, up, forward])          # rows: world -> view
    c5, c6, c7 = basis[:, 0], basis[:, 1], basis[:, 2]   # images of the world x, y, z axes
    c8 = -basis @ camera
    tan_half = np.tan(np.radians(args.fov) / 2)
    p11 = 1 / tan_half
    p00 = p11 * H / W
    p22 = FAR / (FAR - NEAR)
    p23 = -FAR * NEAR / (FAR - NEAR)

    # Pixel rays in world space.
    px = (np.arange(W, dtype=np.float32) + 0.5)
    py = (np.arange(H, dtype=np.float32) + 0.5)
    ndc_x = px / W * 2 - 1
    ndc_y = 1 - py / H * 2                  # window y grows downward (the viewport is flipped like WoW's)
    nx, ny = np.meshgrid(ndc_x, ndc_y)
    dir_view = np.stack([nx / p00, ny / p11, np.ones_like(nx)], axis=-1).astype(np.float32)   # z = 1
    dir_world = dir_view @ basis.astype(np.float32)                                            # R^T * d

    # Beach: the sea is where the terrain is below z = 0, with gentle dunes.
    dune = smooth_noise(rng, 24, 512, 512)
    def terrain(x, y):
        u = np.clip((x + 60) / 120, 0, 0.999) * 511
        v = np.clip((y + 60) / 120, 0, 0.999) * 511
        bump = dune[v.astype(int), u.astype(int)]
        # Land on the left of the picture (small y), sea on the right; the waterline runs along x, 3 yards to the left of the camera.
        if args.far > 0:   # land ahead of the camera (toward -x), waterline `far` yards away, with a wandering coast
            return -args.sea_depth + args.slope * (args.sea_depth / args.slope + (-x - args.far) + 1.5 * np.sin(y * 0.11)) + (bump - 0.5) * 0.7
        return -args.sea_depth + args.slope * (args.sea_depth / args.slope - 3.0 - y) + (bump - 0.5) * 0.7 + 0.35 * np.sin(x * 0.35)

    # March each ray to the terrain (coarse steps, then bisection).
    t_far = 200.0
    steps = 320
    t_lo = np.zeros((H, W), np.float32)
    hit = np.zeros((H, W), bool)
    t_hi = np.full((H, W), t_far, np.float32)
    for i in range(1, steps + 1):
        t = t_far * (i / steps) ** 2
        p = camera + dir_world * t
        below = (p[..., 2] < terrain(p[..., 0], p[..., 1])) & ~hit
        t_hi = np.where(below, t, t_hi)
        t_lo = np.where(below, t_far * ((i - 1) / steps) ** 2, t_lo)
        hit |= below
        if hit.all():
            break
    for _ in range(10):
        mid = (t_lo + t_hi) / 2
        p = camera + dir_world * mid[..., None]
        below = p[..., 2] < terrain(p[..., 0], p[..., 1])
        t_hi = np.where(below, mid, t_hi)
        t_lo = np.where(below, t_lo, mid)
    t_ground = np.where(hit, t_hi, 1e6)          # rays that never reach the ground see the sky
    ground = camera + dir_world * np.minimum(t_ground, 400.0)[..., None]
    ground_view_z = t_ground                      # dir_view has z = 1, so the ray parameter is the view depth

    # Water plane z = 0.
    with np.errstate(divide="ignore", invalid="ignore"):
        t_water = (0 - camera[2]) / dir_world[..., 2]
    water = (t_water > 0) & (t_water < t_ground)
    water_pos = dir_view * t_water[..., None]      # view-space position of the water surface

    # Scene color: sand with noise; under water it is darker and tinted by the game's own (flat) water shading.
    grain = smooth_noise(rng, 512, W, H) * 0.5 + smooth_noise(rng, 96, W, H) * 0.5
    wet = np.clip((0.4 - ground[..., 2]) * 2, 0, 1)
    sand = np.stack([0.62, 0.47, 0.30], -1)[None, None] * (0.75 + 0.5 * grain[..., None]) * (1 - 0.35 * wet[..., None])
    depth_under = np.clip(-ground[..., 2], 0, 4)
    game_water = np.stack([0.30, 0.34, 0.30], -1)[None, None] * (0.75 + 0.4 * grain[..., None])
    mix = np.clip(0.55 + 0.1 * depth_under, 0, 0.85)[..., None]
    color = np.where(water[..., None], sand * (1 - mix) + game_water * mix, sand)
    sky_rows = np.clip(0.5 - (np.arange(H) - H * 0.3) / H, 0, 1)[:, None, None]
    sky_color = np.array([0.55, 0.62, 0.78]) * sky_rows + np.array([0.90, 0.72, 0.55]) * (1 - sky_rows)
    color = np.where((~hit & ~water)[..., None], np.broadcast_to(sky_color, color.shape), color)
    color = np.clip(color, 0, 1)
    bgra = np.dstack([color[..., 2], color[..., 1], color[..., 0], np.ones((H, W), np.float32)])
    (bgra * 255 + 0.5).astype(np.uint8).tofile(out / "scene_before.bin")
    (bgra * 255 + 0.5).astype(np.uint8).tofile(out / "scene_after.bin")

    # Depth: raw = mix(range, P22 + P23 / z), stored as D24 in the low bits of a 32-bit word.
    raw = RANGE[0] + (RANGE[1] - RANGE[0]) * (p22 + p23 / ground_view_z)
    (np.clip(raw, 0, 1) * 16777215 + 0.5).astype(np.uint32).tofile(out / "scene_depth.bin")

    # Water mask: view position (xyz) and alpha.
    mask = np.zeros((H, W, 4), np.float32)
    mask[water, :3] = water_pos[water]
    mask[water, 3] = mix[..., 0][water]
    mask.tofile(out / "mask.bin")
    # The water shader's own output: rgb = the game's flat water color, a = the alpha it blended with (scene = a*W + (1-a)*S, alpha blending).
    own = np.zeros((H, W, 4), np.float16)
    own[water, :3] = game_water[water]
    own[water, 3] = mix[..., 0][water]
    own.tofile(out / "water_color.bin")

    # Camera texture: only texels 9..18 matter to the composite; the rest stay zero.
    cam = np.zeros((20, 4), np.float32)
    plane_normal = c7                                   # world +z in view space
    plane_d = -float(np.dot(c7, c8))
    cam[9] = [*plane_normal, plane_d]
    cam[10] = [NEAR, FAR, p00, p11]
    cam[11] = [0.5, H + 0.5, W, -H]
    cam[12] = [RANGE[0], RANGE[1], 0, 0]
    cam[13] = [1, 0.0, p22, p23]
    cam[18] = [1, 0.0, -float(np.dot(c7, c8)), 0]      # reference plane valid at z 0, camera world z (view origin)
    for i, c in enumerate((c5, c6, c7, c8)):
        cam[14 + i] = [*c, 0]
    cam.tofile(out / "camera.bin")

    # Sky memory: warm horizon into a cooler zenith (u = azimuth, v = elevation).
    v = (np.arange(64) + 0.5) / 64
    sky = np.zeros((64, 128, 4), np.float16)
    horizon = np.array([0.80, 0.66, 0.52]); zenith = np.array([0.30, 0.45, 0.75])
    for row in range(64):
        sky[row, :, :3] = horizon * (1 - v[row] ** 0.5) + zenith * v[row] ** 0.5
    # A sun ahead of the camera (toward -x, where the camera looks) low over the horizon: a bright disc with a glow.
    azimuth = np.arctan2(0.0, -1.0)                       # world direction -x
    elevation = np.radians(args.sun_elevation)
    u = (np.arange(128) + 0.5) / 128
    az = (u - 0.5) * 2 * np.pi
    el = v * (np.pi / 2)
    daz = np.angle(np.exp(1j * (az[None, :] - azimuth)))
    dist = np.sqrt((daz * np.cos(el)[:, None]) ** 2 + (el[:, None] - elevation) ** 2)
    glow = np.exp(-(dist / 0.20) ** 2) * 0.55 + np.exp(-(dist / 0.045) ** 2) * 1.2
    sky[..., :3] = np.clip(sky[..., :3].astype(np.float32) + glow[..., None] * np.array([1.0, 0.85, 0.62]), 0, 4).astype(np.float16)
    sky[..., 3] = 1
    sky.tofile(out / "sky_memory.bin")

    params = {"viewport": [0.5, H + 0.5, W, -H], "depthRange": [RANGE[0], RANGE[1], 0, 0], "vertex": [0, 12, 12, 12], "indices": [0, 2, 0, 3],
              "draw": [0, 0, 0, 15], "options": [1, 0.7, 0, 0], "tone": [0, 0, 0, 0]}
    (out / "capture.json").write_text(json.dumps({"schema": 1, "synthetic": True, "params": params}))
    print("wrote", out, f"(water pixels: {int(water.sum())}, camera height {args.height}, pitch {args.pitch})")


if __name__ == "__main__":
    main()
