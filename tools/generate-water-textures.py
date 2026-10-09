#!/usr/bin/python3
"""Generates shaders/water_detail.bin: the tileable water textures the water composite pass samples.

No third-party art is used (free water-surface textures with a usable license are not tileable or need attribution); both
layers are synthesised here from a fixed seed, so the file is reproducible:

  layer 0  detail normal map   R,G = surface slope (x,y) *.5+.5, scaled so +-1 is three standard deviations; B = 255; A = height
  layer 2  caustics            R,G = two bright-network patterns (multiplied while they scroll apart), B,A = unused
  layer 1  foam pattern        R = lacy foam (uniform value distribution: value > 1-coverage keeps that share), G = second lace pattern, B = bubble specks, A = wind streaks

Every channel is periodic, so it tiles without seams. Mip levels are baked (box filter) so distant water filters smoothly.

File layout (little endian): "SWT1", width, height, layers, levels (uint32 each), then for layer 0..L-1, level 0..M-1 the RGBA8 texels.

Usage: tools/generate-water-textures.py [output.bin] [--preview DIR]
"""
import struct
import sys
from pathlib import Path

import numpy as np

SIZE = 256
LEVELS = 9  # 256 .. 1


def spectrum_noise(rng, exponent, band=(1, SIZE // 2), stretch=(1.0, 1.0), wind=None, wind_weight=0.0):
    """Periodic noise with power ~ k^-exponent (returns a real field, zero mean, unit variance) plus its complex spectrum inputs."""
    k1 = np.fft.fftfreq(SIZE) * SIZE
    kx, ky = np.meshgrid(k1, k1)
    k = np.sqrt((kx * stretch[0]) ** 2 + (ky * stretch[1]) ** 2)
    amp = np.zeros_like(k)
    inside = (k >= band[0]) & (k <= band[1])
    amp[inside] = k[inside] ** (-exponent)
    if wind is not None:
        angle = np.arctan2(ky, kx)
        directional = np.abs(np.cos(angle - wind)) ** 2
        amp *= (1 - wind_weight) + wind_weight * directional * 1.6
    phase = rng.uniform(0, 2 * np.pi, k.shape)
    gauss = rng.normal(size=k.shape)
    spec = amp * gauss * np.exp(1j * phase)
    return spec, kx, ky


def to_real(spec):
    field = np.real(np.fft.ifft2(spec))
    field -= field.mean()
    return field / field.std()


def equalise(field):
    """Rank transform to a uniform 0..1 distribution, so thresholds mean the same coverage on every layer."""
    order = field.ravel().argsort().argsort()
    return (order / (field.size - 1)).reshape(field.shape)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def worley(rng, cells, warp=None):
    """Periodic cellular noise: (F1, F2) distances in units of the cell size. `warp` (two periodic fields, in cells) bends the lookup."""
    points = rng.uniform(0.05, 0.95, (cells, cells, 2))
    px = (np.arange(SIZE) + 0.5) / SIZE * cells
    gx, gy = np.meshgrid(px, px)
    if warp is not None:
        gx = gx + warp[0]
        gy = gy + warp[1]
    cx = np.floor(gx).astype(int)
    cy = np.floor(gy).astype(int)
    f1 = np.full((SIZE, SIZE), 9.0)
    f2 = np.full((SIZE, SIZE), 9.0)
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            nx = cx + ox
            ny = cy + oy
            jitter = points[ny % cells, nx % cells]
            dx = nx + jitter[..., 0] - gx
            dy = ny + jitter[..., 1] - gy
            d = np.sqrt(dx * dx + dy * dy)
            smaller = d < f1
            f2 = np.where(smaller, f1, np.minimum(f2, d))
            f1 = np.where(smaller, d, f1)
    return f1, f2


def ripple_layer(rng):
    wind = 0.35
    spec, kx, ky = spectrum_noise(rng, 1.9, band=(2, 36), wind=wind, wind_weight=0.65)
    height = to_real(spec)
    # Slopes straight from the spectrum keep them exactly periodic: d/dx of the height field.
    spec_h = np.fft.fft2(height)
    scale = 2 * np.pi / SIZE
    sx = np.real(np.fft.ifft2(1j * kx * scale * spec_h))
    sy = np.real(np.fft.ifft2(1j * ky * scale * spec_h))
    sigma = np.sqrt(np.mean(sx * sx + sy * sy) / 2)
    unit = 3 * sigma
    out = np.zeros((SIZE, SIZE, 4))
    out[..., 0] = np.clip(sx / unit, -1, 1) * 0.5 + 0.5
    out[..., 1] = np.clip(sy / unit, -1, 1) * 0.5 + 0.5
    out[..., 2] = 1.0
    out[..., 3] = np.clip(height / 3.0, -1, 1) * 0.5 + 0.5
    return out


def lace(rng, cells, mid_band, fine_band):
    """A foam-like field with a uniform value distribution (so `value > 1 - coverage` selects exactly that share of the area): clumps at the
    mid scale, a network of thin walls (borders between bubble cells) and fine grain. Thresholded low it is thin connected lace, thresholded
    high it is solid foam with round holes, which is how real foam thins out."""
    spec, _, _ = spectrum_noise(rng, 1.6, band=mid_band)
    clumps = equalise(to_real(spec))
    f1, f2 = worley(rng, cells)
    walls = 1 - smoothstep(0.02, 0.28, f2 - f1)
    spec, _, _ = spectrum_noise(rng, 1.0, band=fine_band)
    grain = equalise(to_real(spec))
    return equalise(0.36 * clumps + 0.30 * grain + 0.34 * walls)


def foam_layer(rng):
    main = lace(rng, 36, (3, 28), (10, 110))          # R: the foam pattern the shader thresholds against the local foam amount
    detail = lace(rng, 20, (2, 16), (8, 90))           # G: a second, different pattern used to perturb the threshold
    b1, _ = worley(rng, 90)
    specks = 1 - smoothstep(0.10, 0.34, b1)            # B: tiny bubbles
    spec, _, _ = spectrum_noise(rng, 1.4, band=(1, 120), stretch=(0.18, 1.0))
    streak = smoothstep(0.35, 0.85, equalise(to_real(spec)))   # A: wind streaks
    return np.stack([main, detail, specks, streak], axis=-1)


def caustic_pattern(rng, cells):
    """Bright thin networks (where the borders of bent cells run), like light focused by ripples on a shallow bed. Periodic."""
    spec_x, _, _ = spectrum_noise(rng, 1.8, band=(1, 6))
    spec_y, _, _ = spectrum_noise(rng, 1.8, band=(1, 6))
    warp = (to_real(spec_x) * 0.28, to_real(spec_y) * 0.28)          # bends the cells by a fraction of a cell
    f1, f2 = worley(rng, cells, warp)
    border = f2 - f1
    net = 1 - smoothstep(0.0, 0.30, border)
    return net ** 1.5


def caustic_layer(rng):
    a = caustic_pattern(rng, 12)
    b = caustic_pattern(rng, 19)
    zero = np.zeros_like(a)
    return np.stack([a, b, zero, zero], axis=-1)


def mip_chain(level0):
    chain = [level0]
    while chain[-1].shape[0] > 1:
        p = chain[-1]
        h = p.shape[0] // 2
        chain.append(p.reshape(h, 2, h, 2, 4).mean(axis=(1, 3)))
    return chain


def main():
    args = [a for a in sys.argv[1:]]
    preview = None
    if "--preview" in args:
        i = args.index("--preview")
        preview = Path(args[i + 1])
        del args[i:i + 2]
    output = Path(args[0]) if args else Path(__file__).resolve().parent.parent / "shaders" / "water_detail.bin"
    rng = np.random.default_rng(20260920)
    layers = [ripple_layer(rng), foam_layer(rng), caustic_layer(rng)]
    blob = bytearray(b"SWT1" + struct.pack("<4I", SIZE, SIZE, len(layers), LEVELS))
    for layer in layers:
        for level in mip_chain(layer)[:LEVELS]:
            blob += np.clip(np.rint(level * 255), 0, 255).astype(np.uint8).tobytes()
    output.write_bytes(bytes(blob))
    print(f"wrote {output} ({len(blob)} bytes)")
    if preview:
        from PIL import Image
        preview.mkdir(parents=True, exist_ok=True)
        for name, layer in zip(("ripples", "foam", "caustics"), layers):
            image = np.clip(np.rint(layer * 255), 0, 255).astype(np.uint8)
            tiled = np.tile(image[..., :3], (2, 2, 1))
            Image.fromarray(tiled).save(preview / f"{name}_rgb_tiled.png")
            for c, channel in enumerate("rgba"):
                Image.fromarray(np.tile(image[..., c], (2, 2))).save(preview / f"{name}_{channel}_tiled.png")


if __name__ == "__main__":
    main()
