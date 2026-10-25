#!/usr/bin/env python3
"""Diagnostic root-entry controls; does not replace the main scaling scores."""
import json
import pathlib
import random
import statistics
import sys
import depth_run as d

output = pathlib.Path(sys.argv[1])
assert not output.exists()
records = []
rng = random.Random(20261025)
cells = [(language, depth) for language in ("cpp", "rust", "floor", "current", "hybrid")
         for depth in (0, 100, 1000, 10000)]
for trial in range(3):
    rng.shuffle(cells)
    for language, depth in cells:
        count = 20000 if depth < 1000 else 2000 if depth == 1000 else 200
        rows = d.observe(language, 3, depth, count, rounds=1)
        records.append(dict(language=language, depth=depth, trial=trial, iterations=count,
                            samples=rows, median_ns=statistics.median(r["ns"] / count for r in rows)))
summary = []
for language, depth in sorted(cells):
    values = [r["median_ns"] for r in records if r["language"] == language and r["depth"] == depth]
    summary.append(dict(language=language, depth=depth, median_ns=statistics.median(values),
                        process_medians_ns=values))
result = dict(schema=1, contract="Diagnostic task-boundary control, same compiled kernels and heap-based probe as main study. Includes root state setup and settlement. Three randomized trials. Depth zero measures the root boundary plus scalar return. This is not a subtraction-based replacement score.",
              records=records, summary=summary, commands=d.p.COMMANDS,
              hashes={str(path):d.p.sha(path) for path in [pathlib.Path(__file__),
                d.HERE / "Probe.cpp", *[d.OUT / f"chain.{mode}" for mode in ("floor", "current", "hybrid")],
                d.OUT / "DepthProbe.cpp", d.OUT / "cpp", d.OUT / "rust"]})
output.write_text(json.dumps(result,indent=2)+"\n")
for row in summary:
    print(row["language"],row["depth"],row["median_ns"])
