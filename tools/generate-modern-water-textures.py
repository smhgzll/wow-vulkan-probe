#!/usr/bin/python3
"""Generates shaders/water_modern.bin: the MODERN water textures (same "SWT1" layout as water_detail.bin, see
tools/generate-water-textures.py), 1024^2, 3 layers, full mip chain:

  layer 0  ripple slopes   R,G = surface slope (x,y) *.5+.5, +-1 = three standard deviations; B = 255; A = height
  layer 1  foam            R = main foam (uniform value distribution: value > 1-coverage keeps that share), G = second pattern, B = bubble specks
  layer 2  caustics        R,G,B = caustic light per colour channel (mean ~.5 of the channel's range), A = luma

Two sources:

  (default)     original textures synthesised here from a fixed seed, no third-party art:
                  ripples:  a random height field with the MEASURED radial slope spectrum of Crest's normal map (statistics only, new phases)
                  foam:     domain-warped ridged turbulence (filaments) + three scales of bubble cells + grain, equalised
                  caustics: SIMULATED - vertical light refracted through a periodic ripple surface onto a flat bed, per colour channel with
                            its own spread (water's dispersion, exaggerated about 2x so the fringes show: TUNING), splatted and normalised
  --crest DIR   convert Crest's textures from a local checkout (DIR = crest/Assets/Crest/Crest/Textures): WaveNormals/WaveNormals.png,
                foam.png, Foam2.png, Caustics/Caustics_tex_color.png. Crest is MIT, but it took these files from third parties (its
                SOURCE notes: a BlenderArtists forum post, a Photobucket image linked on the Unreal forums; the foam has none) whose own
                licences are unknown. The script reads them from YOUR checkout; nothing third-party is stored in this repository.

Usage: tools/generate-modern-water-textures.py [output.bin] [--crest DIR] [--preview DIR]
"""
import struct
import sys
from pathlib import Path

import numpy as np

N = 1024
LEVELS = 11  # 1024 .. 1


def equalise(field):
    order = field.ravel().argsort().argsort()
    return (order / (field.size - 1)).reshape(field.shape)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def spectrum_field(rng, exponent, band, n=N, directional=0.0, wind=0.0):
    """Periodic real field, zero mean, unit variance, amplitude ~ k^-exponent inside the band (cycles per tile)."""
    k1 = np.fft.fftfreq(n) * n
    kx, ky = np.meshgrid(k1, k1)
    k = np.hypot(kx, ky)
    amp = np.zeros_like(k)
    inside = (k >= band[0]) & (k <= band[1])
    amp[inside] = k[inside] ** (-exponent)
    if directional > 0:
        amp *= (1 - directional) + directional * np.abs(np.cos(np.arctan2(ky, kx) - wind)) ** 2 * 1.6
    spec = amp * (rng.normal(size=k.shape) + 1j * rng.normal(size=k.shape))
    field = np.real(np.fft.ifft2(spec))
    return field / field.std(), kx, ky


def slopes(height, kx, ky):
    spec = np.fft.fft2(height)
    scale = 2 * np.pi / height.shape[0]
    return np.real(np.fft.ifft2(1j * kx * scale * spec)), np.real(np.fft.ifft2(1j * ky * scale * spec))


# Radial slope power per Fourier mode of Crest's WaveNormals.png (1024^2), normalised to the first bin: MEASURED 2026-09-29 (it is isotropic,
# x/y power 1.10; per octave the slope power is flat from 1 to ~60 cycles per tile and falls off beyond - features down to ~17 texels). Our
# ripples are synthesised with this spectrum and new random phases: its statistics, none of its pixels. (Our first spectrum ran fine noise up
# to 300 cycles; the mip filter averaged it into a smooth, regular look.)
CREST_SLOPE_SPECTRUM = [(1.4, 1), (2.4, 0.5043), (3.5, 0.1887), (4.5, 0.08027), (5.5, 0.05329), (6.5, 0.04547), (7.5, 0.04031), (8.5, 0.02899),
                        (9.9, 0.02187), (12.0, 0.01254), (14.0, 0.01167), (16.4, 0.008918), (19.4, 0.005912), (22.9, 0.004614), (26.9, 0.00304),
                        (31.4, 0.002168), (36.9, 0.001355), (42.9, 0.00109), (50.3, 0.0006274), (59.3, 0.000393), (69.3, 0.0002234),
                        (81.2, 0.0001321), (95.2, 6.284e-05), (111.6, 2.7e-05), (131.1, 8.674e-06), (154.0, 2.045e-06), (180.9, 4.875e-07),
                        (212.3, 2.858e-07), (249.2, 2.138e-07), (292.6, 1.462e-07), (343.4, 1.001e-07), (402.7, 7.286e-08), (472.5, 6.951e-08)]


def ripple_layer(rng):
    k1 = np.fft.fftfreq(N) * N
    kx, ky = np.meshgrid(k1, k1)
    k = np.hypot(kx, ky)
    table_k = np.log([t[0] for t in CREST_SLOPE_SPECTRUM])
    table_p = np.log([t[1] for t in CREST_SLOPE_SPECTRUM])
    slope_power = np.where(k >= 1, np.exp(np.interp(np.log(np.maximum(k, 1)), table_k, table_p, right=table_p[-1])), 0.0)
    # height amplitude from the slope power: slope = i (2 pi k / N) height
    amp = np.where(k >= 1, np.sqrt(slope_power) / np.maximum(2 * np.pi * k / N, 1e-9), 0.0)
    spec = amp * (rng.normal(size=k.shape) + 1j * rng.normal(size=k.shape))
    height = np.real(np.fft.ifft2(spec))
    sx, sy = slopes(height, kx, ky)
    sigma = np.sqrt(np.mean(sx * sx + sy * sy) / 2)
    unit = 3 * sigma
    out = np.zeros((N, N, 4))
    out[..., 0] = np.clip(sx / unit, -1, 1) * 0.5 + 0.5
    out[..., 1] = np.clip(sy / unit, -1, 1) * 0.5 + 0.5
    out[..., 2] = 1.0
    out[..., 3] = np.clip(height / height.std() / 3.0, -1, 1) * 0.5 + 0.5
    return out


def worley_f1f2(rng, cells, warp=None):
    """Periodic Worley distances (F1, F2) in cell units, jittered points, optional warp (in cells)."""
    pts = rng.uniform(0, 1, (cells, cells, 2))
    y, x = np.meshgrid(np.arange(N), np.arange(N), indexing="ij")
    u = x / N * cells
    v = y / N * cells
    if warp is not None:
        u = u + warp[0]
        v = v + warp[1]
    iu, iv = np.floor(u).astype(int), np.floor(v).astype(int)
    f1 = np.full((N, N), 9.0)
    f2 = np.full((N, N), 9.0)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cu, cv = iu + dx, iv + dy
            p = pts[cv % cells, cu % cells]
            d = np.hypot(cu + p[..., 0] - u, cv + p[..., 1] - v)
            f2 = np.where(d < f1, f1, np.minimum(f2, d))
            f1 = np.minimum(f1, d)
    return f1, f2


def foam_layer(rng):
    # Filaments: ridged turbulence, warped, as foam streaks drawn out by the water; bubble cells at three scales (walls between bubbles);
    # grain. Equalised so the shader's coverage threshold keeps exactly the asked share.
    wx, _, _ = spectrum_field(rng, 1.8, (1, 10))
    wy, _, _ = spectrum_field(rng, 1.8, (1, 10))
    warp = (wx * 0.9, wy * 0.9)
    ridge = 0
    for band, weight in (((3, 24), 0.55), ((10, 70), 0.30), ((30, 200), 0.15)):
        f, _, _ = spectrum_field(rng, 1.4, band)
        ridge = ridge + weight * (1 - np.abs(f) / 2.5)
    walls = 0
    for cells, weight in ((14, 0.45), (38, 0.35), (95, 0.20)):
        f1, f2 = worley_f1f2(rng, cells, (warp[0] * cells / 14, warp[1] * cells / 14))
        walls = walls + weight * (1 - smoothstep(0.0, 0.22, f2 - f1))
    grain, _, _ = spectrum_field(rng, 0.9, (80, 400))
    main = equalise(0.5 * equalise(ridge) + 0.38 * equalise(walls) + 0.12 * equalise(grain))
    f1, f2 = worley_f1f2(rng, 24, warp)
    second = equalise(0.6 * (1 - smoothstep(0.0, 0.3, f2 - f1)) + 0.4 * equalise(ridge[::-1, :]))
    b1, _ = worley_f1f2(rng, 150)
    specks = 1 - smoothstep(0.08, 0.30, b1)
    return np.stack([main, second, specks, np.full((N, N), 0.5)], axis=-1)


def caustic_layer(rng):
    # A periodic ripple surface over a flat bed; vertical light is bent by the local slope (small-angle Snell: deviation = slope * (1 - 1/n))
    # and travels a depth to the bed, where every source sample lands (3x3 supersampled) and is splatted with bilinear weights. Where the
    # mapping folds, light piles up into the bright lines of real caustics. The depth is chosen so a fair share of the surface focuses.
    h, kx, ky = spectrum_field(rng, 2.2, (2, 18))
    sx, sy = slopes(h, kx, ky)
    rms = np.sqrt(np.mean(sx * sx + sy * sy))
    sx, sy = sx / rms, sy / rms
    ss = 3
    grid = (np.arange(N * ss) + 0.5) / ss
    gy, gx = np.meshgrid(grid, grid, indexing="ij")
    # slopes at the supersampled points (bilinear from the texture grid)
    def sample(field):
        x0 = np.floor(gx - 0.5).astype(int)
        y0 = np.floor(gy - 0.5).astype(int)
        fx, fy = gx - 0.5 - x0, gy - 0.5 - y0
        a = field[y0 % N, x0 % N]
        b = field[y0 % N, (x0 + 1) % N]
        c = field[(y0 + 1) % N, x0 % N]
        d = field[(y0 + 1) % N, (x0 + 1) % N]
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
    px, py = sample(sx), sample(sy)
    spread = 24.0          # texels of displacement per unit slope: focuses at about this depth (TUNING)
    channels = []
    for dispersion in (0.965, 1.0, 1.035):   # red bends least; about 2x water's real dispersion so the fringes read (TUNING)
        x = gx + px * spread * dispersion
        y = gy + py * spread * dispersion
        x0 = np.floor(x - 0.5).astype(int)
        y0 = np.floor(y - 0.5).astype(int)
        fx, fy = x - 0.5 - x0, y - 0.5 - y0
        acc = np.zeros(N * N)
        for oy, ox, w in ((0, 0, (1 - fx) * (1 - fy)), (0, 1, fx * (1 - fy)), (1, 0, (1 - fx) * fy), (1, 1, fx * fy)):
            idx = ((y0 + oy) % N) * N + (x0 + ox) % N
            acc += np.bincount(idx.ravel(), weights=w.ravel(), minlength=N * N)
        img = acc.reshape(N, N) / (ss * ss)          # mean 1
        channels.append(img)
    img = np.stack(channels, -1)
    # a little blur (the sun is not a point), then tone: 1 -> .45, bright lines saturate softly toward 1
    kernel = np.array([1, 4, 6, 4, 1], float) / 16
    for axis in (0, 1):
        img = sum(np.roll(img, s - 2, axis=axis) * kernel[s] for s in range(5))
    toned = 1 - np.exp(-img * 0.6)
    luma = toned @ np.array([.299, .587, .114])
    return np.concatenate([toned, luma[..., None]], -1)


def load_tiled(path, size, mode):
    from PIL import Image
    im = Image.open(path).convert(mode)
    w, h = im.size
    big = Image.new(mode, (w * 3, h * 3))
    for i in range(3):
        for j in range(3):
            big.paste(im, (i * w, j * h))
    big = big.resize((size * 3, size * 3), Image.LANCZOS)
    return np.asarray(big.crop((size, size, 2 * size, 2 * size))).astype(np.float64) / 255


def crest_layers(directory):
    d = Path(directory)
    n = load_tiled(d / "WaveNormals" / "WaveNormals.png", N, "RGB") * 2 - 1
    nz = np.sqrt(np.clip(1 - n[..., 0] ** 2 - n[..., 1] ** 2, 1e-4, 1))
    sx, sy = -n[..., 0] / nz, -n[..., 1] / nz
    sx -= sx.mean()
    sy -= sy.mean()
    unit = 3 * np.sqrt(np.mean(sx * sx + sy * sy) / 2)
    ripples = np.stack([np.clip(sx / unit, -1, 1) * .5 + .5, np.clip(sy / unit, -1, 1) * .5 + .5, np.ones((N, N)), np.full((N, N), .5)], -1)
    foam = load_tiled(d / "foam.png", N, "RGB") @ np.array([.299, .587, .114])
    foam2 = load_tiled(d / "Foam2.png", N, "L")
    blur = foam.reshape(N // 8, 8, N // 8, 8).mean(axis=(1, 3)).repeat(8, 0).repeat(8, 1)
    specks = np.clip((equalise(foam - blur) - .85) / .15, 0, 1)
    foam_layer_ = np.stack([equalise(foam), equalise(foam2), specks, np.full((N, N), .5)], -1)
    ca = load_tiled(d / "Caustics" / "Caustics_tex_color.png", N, "RGB")
    caustics = np.concatenate([ca, (ca @ np.array([.299, .587, .114]))[..., None]], -1)
    return [ripples, foam_layer_, caustics]


def mip_chain(level0):
    chain = [level0]
    while chain[-1].shape[0] > 1:
        a = chain[-1]
        chain.append((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) / 4)
    return chain


def main():
    args = sys.argv[1:]
    preview = None
    crest = None
    if "--preview" in args:
        i = args.index("--preview")
        preview = Path(args[i + 1])
        del args[i:i + 2]
    if "--crest" in args:
        i = args.index("--crest")
        crest = args[i + 1]
        del args[i:i + 2]
    out = Path(args[0]) if args else Path(__file__).resolve().parent.parent / "shaders" / "water_modern.bin"
    if crest:
        layers = crest_layers(crest)
    else:
        rng = np.random.default_rng(20260929)
        layers = [ripple_layer(rng), foam_layer(rng), caustic_layer(rng)]
    blob = bytearray(b"SWT1" + struct.pack("<4I", N, N, len(layers), LEVELS))
    for layer in layers:
        for level in mip_chain(layer)[:LEVELS]:
            blob += np.clip(np.rint(level * 255), 0, 255).astype(np.uint8).tobytes()
    out.write_bytes(bytes(blob))
    print(f"wrote {out} ({len(blob)} bytes, {'from ' + crest if crest else 'original, synthesised'})")
    if preview:
        from PIL import Image
        preview.mkdir(parents=True, exist_ok=True)
        for name, layer in zip(("ripples", "foam", "caustics"), layers):
            image = np.clip(np.rint(layer * 255), 0, 255).astype(np.uint8)
            Image.fromarray(image[..., :3] if name != "foam" else image[..., 0]).save(preview / f"{name}.png")


if __name__ == "__main__":
    main()
