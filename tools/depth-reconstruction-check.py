#!/usr/bin/env python3
"""Checks per-pixel world-position reconstruction against a real water capture.

Uses the exact math of shaders/depth_world.glsl / camera_math.hpp reconstruct_view_position:
  ndc.xy = ((pixel + .5) - viewport.xy) / viewport.wh * 2 - 1     (viewport height is negative in WoW)
  ndc.z  = (raw - minDepth) / (maxDepth - minDepth)               (WoW world draws: depth range 0..0.94)
  view.z = P23 / (ndc.z - P22), view.xy = ndc.xy * view.z / (P00, P11)
  world  = R^T (view - t)                                           (V = [R | t], columns c5..c8)

Checks, all on the game's own data:
  1. mask pass: reconstructing mask_depth reproduces the independently rasterized view position in mask.bin.
  2. (informational) share of mask pixels where the game's scene depth lies behind the water surface - the
     mask pass replays water WITHOUT the scene depth, so occluded water is in the mask too.
  3. water height: reconstructed world z of the mask is the water mesh's constant height.
Writes PNG visualisations next to the capture (or --out).
"""
import argparse, json, struct, sys
from pathlib import Path
import numpy as np


def load(capture: Path):
    meta = json.loads((capture / "capture.json").read_text())
    W, H = meta["target"]["width"], meta["target"]["height"]
    ubo = (capture / "water_ubo.bin").read_bytes()
    P = np.array(struct.unpack("<16f", ubo[256:320]), dtype=np.float64).reshape(4, 4).T  # columns -> matrix
    V = np.array(struct.unpack("<16f", ubo[336:400]), dtype=np.float64).reshape(4, 4).T
    vp = meta["waterDraw"]["viewport"]
    raw24 = np.fromfile(capture / "scene_depth.bin", dtype=np.uint32).reshape(H, W) & 0xFFFFFF
    scene = raw24.astype(np.float64) / 16777215.0
    mask = np.fromfile(capture / "mask.bin", dtype=np.float32).reshape(H, W, 4).astype(np.float64)
    mask_depth = np.fromfile(capture / "mask_depth.bin", dtype=np.float32).reshape(H, W).astype(np.float64)
    color = np.fromfile(capture / "scene_before.bin", dtype=np.uint8).reshape(H, W, 4)[..., [2, 1, 0]]  # B8G8R8A8
    vertices = (capture / "water_vertices.bin").read_bytes()
    pa = meta["waterDraw"]["positionAttribute"]
    indices = (capture / "water_indices.bin").read_bytes()
    wd = meta["waterDraw"]
    fmt = "<H" if wd["indexType"] == 0 else "<I"
    size = 2 if fmt == "<H" else 4
    used = sorted({struct.unpack_from(fmt, indices, (wd["first"] + i) * size)[0] + wd["vertexOffset"] for i in range(wd["count"])})
    heights = [struct.unpack_from("<f", vertices, i * pa["stride"] + pa["offset"] + 8)[0] for i in used if (i + 1) * pa["stride"] <= len(vertices)]
    return meta, W, H, P, V, vp, scene, mask, mask_depth, color, heights


def reconstruct_view(raw, P, vp, W, H):
    px, py = np.meshgrid(np.arange(W) + .5, np.arange(H) + .5)
    ndc_x = (px - vp[0]) / vp[2] * 2 - 1
    ndc_y = (py - vp[1]) / vp[3] * 2 - 1
    ndc_z = (raw - vp[4]) / (vp[5] - vp[4])
    z = P[2, 3] / (ndc_z - P[2, 2])
    return np.stack([ndc_x * z / P[0, 0], ndc_y * z / P[1, 1], z], axis=-1)


def view_to_world(view, V):
    R, t = V[:3, :3], V[:3, 3]
    return (view - t) @ R  # R^T (v - t), row-vector form


def png(path: Path, rgb: np.ndarray):
    from PIL import Image
    Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    meta, W, H, P, V, vp, scene, mask, mask_depth, color, heights = load(a.capture)
    out = a.out or a.capture
    out.mkdir(parents=True, exist_ok=True)
    minD, maxD = vp[4], vp[5]
    ok = True
    print(f"capture {a.capture.name}: {W}x{H} viewport {vp}")
    print(f"projection P00 {P[0,0]:.6f} P11 {P[1,1]:.6f} P22 {P[2,2]:.9f} P23 {P[2,3]:.9f} "
          f"-> near {-P[2,3]/P[2,2]:.4f} far {-P[2,3]/(P[2,2]-1):.2f} fovY {np.degrees(2*np.arctan(1/P[1,1])):.3f}")
    cam = -V[:3, :3].T @ V[:3, 3]
    print(f"camera world position {cam.round(3)}")

    world_px = scene < maxD - 1e-7
    sky = (scene >= maxD - 1e-7) & (scene < 1.0)
    clear = scene >= 1.0
    print(f"depth classes: world(<{maxD}) {world_px.mean()*100:.1f}%  sky({maxD}..1) {sky.mean()*100:.1f}%  cleared(1.0) {clear.mean()*100:.1f}%")
    hist = np.histogram(scene[~world_px], bins=[maxD - 1e-7, .998, .999, .9999, 1.0 + 1e-9])[0]
    print(f"  non-world raw depth histogram [.94,.998) [.998,.999) [.999,.9999) [.9999,1]: {hist.tolist()}")

    water = mask[..., 3] > 0
    print(f"water pixels {water.sum()}")
    height = float(np.median(heights))
    print(f"water mesh height (vertex z) median {height:.4f} spread {max(heights)-min(heights):.5f}")

    # 1. mask pass round trip
    rec_mask = reconstruct_view(mask_depth, P, vp, W, H)
    err = np.linalg.norm(rec_mask[water] - mask[..., :3][water], axis=-1)
    rel = err / np.maximum(mask[..., 2][water], 1e-6)
    print(f"[1] mask_depth -> view vs rasterized mask.xyz: median {np.median(err):.5f} p99 {np.percentile(err,99):.5f} "
          f"max {err.max():.5f} yd (relative median {np.median(rel):.2e} max {rel.max():.2e})")
    ok &= bool(np.percentile(rel, 99) < 1e-3)

    # 2. depth test consistency with the game's own scene depth
    rec_scene = reconstruct_view(scene, P, vp, W, H)
    zs, zw = rec_scene[..., 2][water & world_px], mask[..., 2][water & world_px]
    # One D24 step near depth z is about z*z*(-P22/P23)*(maxD-minD)^-1/2^24 yards.
    step = zw * zw * P[2, 2] / -P[2, 3] / 16777215.0 / (maxD - minD)
    behind = zs - zw >= -2 * step - 1e-4
    print(f"[2] (info) scene behind the water surface on {behind.mean()*100:.1f}% of {behind.size} mask pixels (rest: water occluded)")
    # What ignoring the 0..0.94 depth range would do to the same mask round trip.
    wrong = reconstruct_view(mask_depth, P, [*vp[:4], 0.0, 1.0], W, H)
    werr = np.abs(wrong[..., 2][water] - mask[..., 2][water]) / np.maximum(mask[..., 2][water], 1e-6)
    print(f"    control: depth range ignored -> relative depth error median {np.median(werr):.2e} (check [1] would fail)")
    wrong = reconstruct_view(mask_depth, P, [vp[0] - .5, vp[1] - .5, *vp[2:]], W, H)
    werr = np.linalg.norm(wrong[water] - mask[..., :3][water], axis=-1)
    print(f"    control: half-pixel viewport offset ignored -> median error {np.median(werr):.5f} yd (vs {np.median(err):.5f})")

    # 3. world height of the reconstructed water surface
    world_mask = view_to_world(rec_mask, V)
    dz = world_mask[..., 2][water] - height
    print(f"[3] reconstructed water surface world z - mesh height: median {np.median(dz):.5f} max |dz| {np.abs(dz).max():.5f} yd")
    ok &= bool(np.abs(dz).max() < .05)

    world = view_to_world(rec_scene, V)
    wz = world[..., 2][world_px]
    dist = np.linalg.norm(rec_scene, axis=-1)
    print(f"scene world z range {np.percentile(wz,1):.2f} .. {np.percentile(wz,99):.2f}, distance p50 {np.median(dist[world_px]):.1f} max {dist[world_px].max():.1f}")

    # Visualisations (same palette as the in-game RECON modes).
    base = color.astype(np.float64)
    grid = base.copy()
    wx, wy = world[..., 0], world[..., 1]
    for spacing, width, rgb in ((1.0, .04, (255, 255, 255)), (10.0, .12, (255, 200, 40))):
        lx = np.abs((wx / spacing + .5) % 1 - .5) * spacing < width
        ly = np.abs((wy / spacing + .5) % 1 - .5) * spacing < width
        line = (lx | ly) & world_px
        grid[line] = grid[line] * .3 + np.array(rgb) * .7
    contour = (np.abs((world[..., 2] + .5) % 1 - .5) < .03) & world_px
    grid[contour] = grid[contour] * .3 + np.array((60, 220, 255)) * .7
    png(out / "recon_world_grid.png", grid)
    wc = np.where(world_px[..., None], ((world / 16.0) % 1.0) * 255, 0)
    png(out / "recon_world_position.png", wc)
    n = np.cross(np.gradient(world, axis=1), np.gradient(world, axis=0))
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
    png(out / "recon_normals.png", np.where(world_px[..., None], (n * .5 + .5) * 255, 0))
    heat = np.clip(dist / 200.0, 0, 1)
    png(out / "recon_distance.png", np.where(world_px[..., None], np.stack([heat, 1 - np.abs(heat * 2 - 1), 1 - heat], -1) * 255, 0))
    cls = np.zeros((H, W, 3))
    cls[world_px] = np.stack([np.clip(1 - dist[world_px] / 300, 0, 1)] * 3, -1) * 255
    cls[sky] = (40, 90, 255)
    cls[clear] = (255, 0, 255)
    png(out / "recon_depth_classes.png", cls)
    print(f"wrote recon_*.png to {out}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
