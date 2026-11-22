
For a CSV and JSON report from a native run, use
`python tools/analyze_results.py <run>/timings.csv --output <run>/analysis`.
The report groups samples by scene, sampling mode and resolution. The same
script also accepts legacy `frames.csv` files produced by `tools/export.py`.
