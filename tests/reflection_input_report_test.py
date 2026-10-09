import importlib.util
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("reflection_report", Path(__file__).resolve().parents[1] / "tools/reflection-input-report.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class ReportTests(unittest.TestCase):
    def test_projection_algebra_and_handedness(self):
        near, far = .2, 800
        sy = 1 / math.tan(math.radians(54) / 2)
        p = [[sy / (16/9), 0, 0, 0], [0, sy, 0, 0],
             [0, 0, far / (far - near), 1], [0, 0, -far * near / (far - near), 0]]
        m = [[-1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [10, 20, 30, 1]]
        matrices = {"c0_to_c3": p, "c5_to_c8": m}
        result = tool.matrix_diagnostics(matrices)
        self.assertTrue(result["projection"]["recognized"])
        self.assertAlmostEqual(result["projection"]["nearFromStoredCoefficients"], near)
        self.assertAlmostEqual(result["projection"]["farFromStoredCoefficients"], far)
        self.assertAlmostEqual(result["projection"]["verticalFovDegrees"], 54)
        self.assertAlmostEqual(result["projection"]["aspect"], 16/9)
        self.assertEqual(result["preProjectionTransform"]["determinant"], -1)
        self.assertEqual(result["preProjectionTransform"]["approximateInverseOriginInInputSpace"], [10, -20, -30])
        self.assertFalse(result["cameraSemanticsVerified"])
        self.assertFalse(result["originalDrawValuesVerified"])
        # Transposed, oblique, infinite, reversed, malformed and non-finite data
        # must not silently produce this finite symmetric camera model.
        for bad in ([list(row) for row in zip(*p)], p[:3]):
            self.assertFalse(tool.matrix_diagnostics({"c0_to_c3": bad})["projection"]["recognized"])
        for column, row, value in ((2, 0, .1), (2, 2, 1), (2, 2, -.1), (0, 0, None), (1, 1, float("nan"))):
            bad = [c[:] for c in p];bad[column][row] = value
            self.assertFalse(tool.matrix_diagnostics({"c0_to_c3": bad})["projection"]["recognized"])
        m[0][0] = -2
        self.assertFalse(tool.matrix_diagnostics(matrices)["preProjectionTransform"]["orthogonalAffine"])

    def test_water_viewport_and_constants(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "draw-state.json").write_text(json.dumps({
                "effectiveState": {"validBits": 1, "depthWrite": 0, "blend": 1},
                "viewport": [.5, 1080.5, 1920, -1080, 0, .94], "pixel": [960, 540],
                "sceneDepthSample": {"ready": True, "raw": .47}}))
            (root / "manifest.json").write_text(json.dumps({"shaders": [{"stage": "vertex", "hash": tool.WATER_VS}]}))
            data = bytearray(896)
            for i in range(9):
                struct.pack_into("<4f", data, 256 + i * 16, i, i + 1, i + 2, i + 3)
            (root / "uniform.bin").write_bytes(data)
            entry = {"set": 2, "binding": 0, "element": 0, "range": 896, "status": "saved", "file": "uniform.bin",
                     "captureMoment": "gpu-end-of-render-pass; original draw values not guaranteed"}
            (root / "buffers.json").write_text(json.dumps({"buffers": [entry]}))
            result = tool.report(root)
            self.assertEqual(result["viewportCoordinates"]["xyNdcAtPixelCenter"], [0, 0])
            self.assertEqual(result["viewportCoordinates"]["sceneDepthUnitRange"], .5)
            self.assertEqual(result["waterShaderTransforms"]["columns"]["c5_to_c8"][0], [5, 6, 7, 8])
            self.assertFalse(result["cameraVerified"])
            self.assertFalse(result["waterSurfaceDepthAvailable"])
            self.assertEqual(result["uniforms"][0]["captureMoment"], entry["captureMoment"])
            self.assertIn("end-of-render-pass", result["uniformStatus"])
            entry["status"] = "unavailable"
            entry["reason"] = "buffer-not-proven-gpu-read-only"
            (root / "buffers.json").write_text(json.dumps({"buffers": [entry]}))
            self.assertNotIn("waterShaderTransforms", tool.report(root))
            self.assertIn("No uniform bytes saved", tool.report(root)["uniformStatus"])
            entry["status"] = "saved"
            (root / "buffers.json").write_text(json.dumps({"buffers": [entry]}))
            struct.pack_into("<f", data, 256, float("nan"));(root / "uniform.bin").write_bytes(data)
            self.assertIsNone(tool.report(root)["waterShaderTransforms"]["columns"]["c0_to_c3"][0][0])
            (root / "buffers.json").unlink()
            self.assertNotIn("waterShaderTransforms", tool.report(root))
            (root / "draw-state.json").write_text("{}")
            self.assertNotIn("viewportCoordinates", tool.report(root))


if __name__ == "__main__":
    unittest.main()
