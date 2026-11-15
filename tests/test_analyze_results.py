import importlib.util
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).parents[1] / "tools" / "analyze_results.py"
SPEC = importlib.util.spec_from_file_location("analyze_results", MODULE)
analyze = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyze)


def row(mode, elapsed, preset="saturn", width="640", height="360", iteration="0"):
    return {
        "preset": preset, "mode": mode, "width": width, "height": height,
        "iteration": iteration, "kernel_ms": str(elapsed),
        "shading_calls": "10", "coverage_tests": "20", "buffer_bytes": "30",
    }


class AnalyzeTests(unittest.TestCase):
    def test_grouped_statistics(self):
        result = analyze.summarize_rows([
            row("SSAA4", 3), row("SSAA4", 1), row("SSAA4", 2),
            row("Single", 0.5),
        ])
        self.assertEqual(len(result), 2)
        ssaa = next(item for item in result if item["mode"] == "SSAA4")
        self.assertEqual(ssaa["samples"], 3)
        self.assertEqual(ssaa["median_ms"], 2)
        self.assertEqual(ssaa["p95_ms"], 3)

    def test_reject_invalid_measurements(self):
        with self.assertRaises(ValueError):
            analyze.summarize_rows([row("Single", float("nan"))])
        with self.assertRaises(ValueError):
            analyze.summarize_rows([{"mode": "Single"}])

    def test_write_reports(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "frames.csv"
            import csv
            fields = sorted(analyze.REQUIRED)
            with source.open("w", newline="", encoding="utf-8-sig") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                writer.writerow(row("Single", 1.25))
            out = Path(temp) / "out"
            result = analyze.analyze_file(source, out)
            self.assertEqual(result[0]["median_ms"], 1.25)
            self.assertTrue((out / "summary.csv").exists())
            self.assertTrue((out / "summary.json").exists())


if __name__ == "__main__":
    unittest.main()
