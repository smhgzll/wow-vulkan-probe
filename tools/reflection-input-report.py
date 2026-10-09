#!/usr/bin/env python3
"""Compare Inspector exports and decode observed constants, without camera guesses."""
import argparse
import json
import math
from pathlib import Path
import struct

WATER_VS = "2fce10e0177c7507"


def finite(value):
    return value if math.isfinite(value) else None


def read_json(path):
    return json.loads(path.read_text()) if path.exists() else {}


def matrix_diagnostics(matrices):
    """Validate algebra for the known shader registers, never guess other UBOs."""
    result = {"cameraSemanticsVerified": False, "originalDrawValuesVerified": False}
    p = matrices.get("c0_to_c3", [])
    m = matrices.get("c5_to_c8", [])

    def finite_matrix(columns):
        return (len(columns) == 4 and all(len(c) == 4 for c in columns)
                and all(isinstance(v, (int, float)) and math.isfinite(v) for c in columns for v in c))

    result["projection"] = {"recognized": False}
    if finite_matrix(p):
        sx, sy, a, b = p[0][0], p[1][1], p[2][2], p[3][2]
        allowed = {(0, 0), (1, 1), (2, 2), (3, 2), (2, 3)}
        symmetric = all(abs(p[c][r]) <= 1e-7 for c in range(4) for r in range(4) if (c, r) not in allowed)
        if symmetric and sx > 0 and sy > 0 and a > 1 and b < 0 and abs(p[2][3] - 1) <= 1e-7:
            near, far = -b / a, -b / (a - 1)
            fov = math.degrees(2 * math.atan(1 / sy))
            aspect = sy / sx
            if all(math.isfinite(v) for v in (near, far, fov, aspect)) and far > near > 0:
                result["projection"] = {
                    "recognized": True,
                    "form": "finite symmetric perspective; visible +Z; NDC depth 0..1",
                    "nearFromStoredCoefficients": near,
                    "farFromStoredCoefficients": far,
                    "verticalFovDegrees": fov,
                    "aspect": aspect,
                    "ndcDepthEquation": "d = A + B / z; A=c2.z, B=c3.z",
                    "depthCoefficients": [a, b],
                    "meaning": "algebra of stored float32 coefficients; not an automatic runtime camera profile",
                }

    result["preProjectionTransform"] = {"orthogonalAffine": False}
    if finite_matrix(m):
        error = max(abs(sum(m[i][k] * m[j][k] for k in range(3)) - (i == j))
                    for i in range(3) for j in range(3))
        determinant = (m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2])
                       - m[1][0] * (m[0][1] * m[2][2] - m[2][1] * m[0][2])
                       + m[2][0] * (m[0][1] * m[1][2] - m[1][1] * m[0][2]))
        affine = max(abs(m[i][3]) for i in range(3)) <= 1e-7 and abs(m[3][3] - 1) <= 1e-7
        if affine and error < 1e-5 and abs(abs(determinant) - 1) < 1e-5:
            result["preProjectionTransform"] = {
                "orthogonalAffine": True,
                "orthogonalityMaxError": error,
                "determinant": determinant,
                "approximateInverseOriginInInputSpace": [-sum(m[i][k] * m[3][k] for k in range(3)) for i in range(3)],
                "meaning": "input-space origin candidate; world-space camera identity is unverified",
            }
    return result


def report(directory):
    directory = Path(directory)
    state = read_json(directory / "draw-state.json")
    manifest = read_json(directory / "manifest.json")
    buffers = read_json(directory / "buffers.json")
    result = {
        "directory": str(directory),
        "draw": state.get("draw"),
        "scope": state.get("scope"),
        "effectiveState": state.get("effectiveState", {}),
        "shaders": manifest.get("shaders", []),
        "textures": manifest.get("textures", []),
        "cameraVerified": False,
        "waterSurfaceDepthAvailable": False,
        "waterSSRImplemented": False,
        "planarScenePassImplemented": False,
        "uniforms": [],
    }
    # Reverse the actual Vulkan viewport transform, not a guessed camera.
    # This is sampled scene depth; a transparent candidate may not have written it.
    viewport = state.get("viewport", [])
    pixel = state.get("pixel", [])
    depth = state.get("sceneDepthSample", {})
    if (state.get("effectiveState", {}).get("validBits", 0) & 1
            and len(viewport) == 6 and len(pixel) == 2
            and all(isinstance(v, (int, float)) and math.isfinite(v) for v in viewport + pixel)):
        x, y, w, h, lo, hi = viewport
        if w and h and hi != lo:
            result["viewportCoordinates"] = {
                "xyNdcAtPixelCenter": [2 * (pixel[0] + .5 - x) / w - 1,
                                       2 * (pixel[1] + .5 - y) / h - 1],
                "depthRange": [lo, hi],
                "meaning": "inverse viewport only; scene depth is not selected water depth",
            }
            raw = depth.get("raw")
            if depth.get("ready") and isinstance(raw, (int, float)) and math.isfinite(raw):
                result["viewportCoordinates"]["sceneDepthUnitRange"] = (raw - lo) / (hi - lo)
    water_shader = any(s.get("stage") == "vertex" and s.get("hash") == WATER_VS
                       for s in manifest.get("shaders", []))
    for entry in buffers.get("buffers", []):
        out = dict(entry)
        result["uniforms"].append(out)
        if entry.get("status") != "saved":
            continue
        file = entry.get("file", "")
        if not file or Path(file).name != file:
            out["decodeError"] = "invalid snapshot filename"
            continue
        path = directory / file
        if not path.is_file() or path.stat().st_size > 65536:
            out["decodeError"] = "missing or oversized snapshot"
            continue
        data = path.read_bytes()
        if len(data) != entry.get("range"):
            out["decodeError"] = "snapshot size differs from descriptor range"
            continue
        out["float4ByByteOffset"] = [
            {"offset": offset, "values": [finite(v) for v in struct.unpack_from("<4f", data, offset)]}
            for offset in range(0, len(data) - 15, 16)
        ]
        if water_shader and entry.get("set") == 2 and entry.get("binding") == 0 and entry.get("element") == 0:
            # Layout and register use were established from this exact VS SPIR-V.
            # Columns multiply position components; no world/view/projection names
            # are assigned until the runtime matrices have been validated.
            matrices = {}
            for name, regs in (("c0_to_c3", [0, 1, 2, 3]), ("c5_to_c8", [5, 6, 7, 8])):
                if len(data) >= 256 + 16 * (max(regs) + 1):
                    matrices[name] = [[finite(v) for v in struct.unpack_from("<4f", data, 256 + 16 * reg)] for reg in regs]
            result["waterShaderTransforms"] = {
                "layout": "column vectors; float registers start at descriptor byte 256, stride 16",
                "equation": "t=c5*x+c6*y+c7*z+c8; clip=c0*t.x+c1*t.y+c2*t.z+c3*t.w",
                "columns": matrices,
                "semanticsVerified": False,
                "diagnostics": matrix_diagnostics(matrices),
            }
    if not buffers:
        result["uniformStatus"] = "This export predates uniform byte capture; runtime constants cannot be recovered from descriptor addresses."
    else:
        saved = [u for u in result["uniforms"] if u.get("status") == "saved"]
        result["uniformStatus"] = (
            "Per-binding captureMoment identifies host-before-submit or GPU end-of-render-pass snapshots; camera and original draw values require validation."
            if saved else "No uniform bytes saved; see each binding's reason."
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        text = json.dumps({"schema": 1, "exports": [report(p) for p in args.directories]}, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(text)
        else:
            print(text, end="")
    except (OSError, ValueError, TypeError) as exc:
        parser.exit(1, f"Cannot analyze export: {exc}\n")


if __name__ == "__main__":
    main()
