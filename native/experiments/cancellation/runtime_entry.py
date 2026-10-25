#!/usr/bin/env python3
"""Paired runtime comparison: identical cleaned harness object and native IR.

Requires depth_run.py and entry_ablation.py build artifacts. Pass a frozen
pre-change task-core archive, then a new output JSON path. Never overwrites it.
"""
import datetime
import json
import pathlib
import random
import statistics
import sys
import entry_ablation as e

d = e.d


def main():
    baseline, output = map(pathlib.Path, sys.argv[1:])
    assert baseline.is_file() and not output.exists()
    current = d.ROOT / "build/native/native/libkalandra-task-core-optimized.a"
    obj = e.OUT / "runtime-entry.o"
    d.p.run([d.p.CLANG, "-std=c++20", "-O2", "-ffixed-x28", "-I", d.HERE,
             "-c", e.OUT / "stack-clean.cpp", "-o", obj])
    cancellations = []
    tail = d.ROOT / "benchmarks/.build/cancellation/tail.hybrid.ll"
    for name, archive in (("runtime-before", baseline), ("runtime-after", current)):
        for kernel, suffix in ((d.OUT / "chain.hybrid.ll", ""), (tail, ".tail")):
            binary = e.OUT / (name + suffix)
            d.p.run([d.p.CLANG, "-O2", kernel, obj, d.OUT / "enter.o", archive,
                     "-lpthread", "-o", binary])
            assert e.prep.extents(binary) == e.prep.extents(
                d.OUT / "chain.hybrid" if not suffix else e.OUT / "stack-clean.tail")
        result = json.loads(d.p.run([e.OUT / (name + ".tail"), "hybrid", 100000000, 100, 1, 1]))
        assert result["cancelled"] == 100 and result["checksum"] == 0
        cancellations.append(dict(version=name, result=result))
    versions = ("runtime-before", "runtime-after", "cpp", "rust")
    checks = [e.observe(v, depth, 1, seed) for v in versions
              for depth in (0, 1, 100, 1000, 10000) for seed in (1, 17, 31)]
    samples = []
    rng = random.Random(20261025)
    for block in range(7):
        depths = [0, 100, 1000, 10000]
        rng.shuffle(depths)
        for depth in depths:
            count = {0: 100000, 100: 20000, 1000: 2000, 10000: 200}[depth]
            order = list(versions)
            rng.shuffle(order)
            for name in order + order[::-1]:
                samples.append(dict(block=block, **e.observe(name, depth, count)))
        print("Runtime comparison block", block + 1, "complete", flush=True)
    summary = []
    for depth in (0, 100, 1000, 10000):
        times = {v: statistics.median(row["ns"] / row["count"] for row in samples
                                     if row["version"] == v and row["depth"] == depth)
                 for v in versions}
        row = dict(depth=depth, median_ns=times,
                   after_over_before=times["runtime-after"] / times["runtime-before"],
                   after_over_cpp=times["runtime-after"] / times["cpp"],
                   after_over_rust=times["runtime-after"] / times["rust"])
        summary.append(row)
        print(json.dumps(row), flush=True)
    output.write_text(json.dumps(dict(timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        contract="Same freshly compiled stack-clean harness object, unchanged hybrid kernel IR, two full-batch warmups, seven randomized mirrored blocks. Ordinary C++/Rust references. Explicit allocation hook override remains in harness. Clockless cancellation fields do not measure latency. This is an experimental task-entry comparison, not production hybrid integration.",
        verification=checks, cancellation=cancellations, samples=samples, summary=summary,
        commands=d.p.COMMANDS, hashes={d.p.key(path): d.p.sha(path) for path in (
            pathlib.Path(__file__), baseline, current, obj, e.OUT / "stack-clean.cpp",
            d.OUT / "chain.hybrid.ll", tail, d.OUT / "cpp", d.OUT / "rust")}), indent=2) + "\n")


if __name__ == "__main__":
    main()
