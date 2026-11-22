"""Summarize per-frame benchmark CSV records into grouped median/P95 reports."""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

REQUIRED = {
    "preset", "mode", "width", "height", "iteration", "kernel_ms",
    "shading_calls", "coverage_tests", "buffer_bytes",
}
NATIVE_COLUMNS = {"scene", "mode", "run", "render_ms", "width", "height"}
GROUP_KEYS = ("preset", "mode", "width", "height")


def percentile_nearest_rank(values, percentile):
    if not values:
        raise ValueError("percentile requires at least one value")
    if not 0 < percentile <= 1:
        raise ValueError("percentile must be in (0, 1]")
    ordered = sorted(values)
    index = math.ceil(percentile * len(ordered)) - 1
    return ordered[index]


def summarize_rows(rows, native=False):
    groups = defaultdict(list)
    for row in rows:
        missing = (NATIVE_COLUMNS if native else REQUIRED).difference(row)
        if missing:
            raise ValueError("missing benchmark columns: " + ", ".join(sorted(missing)))
        time_column = "render_ms" if native else "kernel_ms"
        try:
            elapsed = float(row[time_column])
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{time_column} must be numeric") from exc
        if not math.isfinite(elapsed) or elapsed < 0:
            raise ValueError(f"{time_column} must be finite and non-negative")
        group_keys = ("scene", *GROUP_KEYS[1:]) if native else GROUP_KEYS
        key = tuple(str(row[name]) for name in group_keys)
        groups[key].append(elapsed)

    output = []
    for key in sorted(groups):
        values = groups[key]
        output.append({
            "preset": key[0],
            "mode": key[1],
            "width": int(key[2]),
            "height": int(key[3]),
            "samples": len(values),
            "median_ms": statistics.median(values),
            "p95_ms": percentile_nearest_rank(values, 0.95),
            "min_ms": min(values),
            "max_ms": max(values),
        })
    return output


def analyze_file(input_csv, output_dir):
    with Path(input_csv).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError("benchmark CSV is empty")
        native = NATIVE_COLUMNS.issubset(reader.fieldnames)
        missing = set() if native else REQUIRED.difference(reader.fieldnames)
        if missing:
            raise ValueError("missing benchmark columns: " + ", ".join(sorted(missing)))
        summary = summarize_rows(reader, native=native)

    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    with (out / "summary.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        fields = ["preset", "mode", "width", "height", "samples",
                  "median_ms", "p95_ms", "min_ms", "max_ms"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary)
    (out / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, allow_nan=False),
        encoding="utf-8",
    )
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_csv", help="Native timings.csv or legacy frames.csv")
    parser.add_argument("--output", default="outputs/analysis", help="output directory")
    args = parser.parse_args()
    summary = analyze_file(args.input_csv, args.output)
    print(f"Summarized {len(summary)} benchmark groups into {args.output}")


if __name__ == "__main__":
    main()
