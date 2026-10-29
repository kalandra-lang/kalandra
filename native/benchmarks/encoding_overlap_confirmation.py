"""Longer-batch replication using the exact, already-verified overlap artifacts.

Primary evidence is retained unchanged. This measures all cases again, including
single-command controls with inconsistent short-batch results; it does not select
or discard process samples. No compilation occurs in this runner.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "fair"))
import protocol as p

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--evidence", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--study", choices=("paired", "control"), required=True)
parser.add_argument("--blocks", type=int, default=7)
parser.add_argument("--target-ms", type=float, default=100)
args = parser.parse_args()
assert not args.output.exists()
for flag in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "DYLD_INSERT_LIBRARIES", "KALANDRA_GPU_PROFILE"):
    assert os.getenv(flag) in (None, "", "0"), flag
parent = json.loads(args.evidence.read_text())


def verify():
    for collection in ("artifact_sha256", "source_sha256"):
        for file, digest in parent[collection].items():
            assert p.sha(p.ROOT / file) == digest, file


verify()
assert all(row["bit_identical_to_baseline"] for row in parent["verification"]
           if row["backend"] in ("old", "new", "control"))
build = p.ROOT / "benchmarks/.build/gpu-encoding-overlap-2026-10-29/acceptance"
baseline = p.ROOT / "benchmarks/.build/gpu-transpose-view-2026-10-29/acceptance"
bundle = p.ROOT / "benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle"
for file in (build / "gpu", baseline / "gpu"):
    assert p.key(file) in parent["artifact_sha256"]
shapes = sorted({tuple(row["group"][:2]) for row in parent["pairs"]})
assert len(shapes) == 9


def invoke(cell, count, rounds):
    version, m, d, mode = cell
    if version == "baseline" and args.study == "paired":
        command = [baseline / "gpu", bundle, m, d, count, rounds, mode, 0, 0, 1, 1, 1, 1, 1]
    else:
        command = [build / "gpu", bundle, m, d, count, rounds, mode, 0, 0, 1, 1, 1, 1, 1,
                   int(version == "current")]
    rows = p.observations(p.run(command), m * d * 4, count, rounds)
    commands = 5 if (m, d) == (64, 512) else 3 if (m, d) in ((16, 512), (64, 256)) else 1
    assert all(row["submissions"] == commands * count for row in rows)
    assert all(row["gpu_ns"] == row["encoding_ns"] == row["dispatches"] == 0 for row in rows)
    return rows


cells = [(version, m, d, mode) for m, d in shapes for mode in range(3)
         for version in ("baseline", "current")]
result = p.measure_paired(cells, invoke, lambda c: c[1] * c[2] * 4,
                          blocks=args.blocks, target_ms=args.target_ms,
                          adaptive=True, max_iterations=5000)
verify()
result.update(contract=__doc__, study="confirm-" + args.study,
              verification=parent["verification"], qualification=parent["qualification"],
              task_archive_equivalence=parent["task_archive_equivalence"],
              parent_evidence_sha256=p.sha(args.evidence))
p.evidence(args.output, result, [Path(__file__), Path(p.__file__), args.evidence],
           [p.ROOT / file for file in parent["artifact_sha256"]], build / "kalandra")
