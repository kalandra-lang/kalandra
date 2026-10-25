#!/usr/bin/env python3
"""Reproducible local experiment, with exact numerical and cleanup checks."""
import hashlib
import json
import os
import pathlib
import random
import statistics
import subprocess
import sys
import time
import run as probe

ROOT = probe.ROOT
sys.path.insert(0, str(ROOT / "native/benchmarks/fair"))
from source_run import expected

KINDS = ("polynomial", "branches", "euclid", "tail", "mutual", "owned")


def reference(kind, depth, seed):
    if kind in KINDS[:3]:
        return expected(KINDS.index(kind), depth, seed)
    value = seed
    if kind == "owned":
        for _ in range(depth):
            value = (value * 14 + 1) % 1000003
        return float(value + 2)
    for step in range(depth):
        value = (value * 13 + 7) % 1000003 if kind == "tail" or step % 2 else (value * 7 + 11) % 1000003
    return float(value)


def invoke(kind, mode, depth, count, cancel=False, owned=False, seed=17, fault=None, borrow=False):
    args = [probe.OUT / f"{kind}.{mode}", mode, depth, count, int(cancel), int(owned)]
    if fault is not None:
        args += [fault, 2]
    env = dict(os.environ, KALANDRA_PROBE_SEED=str(seed))
    if borrow:
        env["KALANDRA_PROBE_BORROW_REGISTER"] = "1"
    process = subprocess.run(list(map(str, args)), text=True, capture_output=True,
                             env=env, timeout=30)
    if process.returncode:
        raise RuntimeError(f"{args}: {process.returncode}\n{process.stderr}")
    row = json.loads(process.stdout)
    if fault is None and not cancel:
        assert row["checksum"] == reference(kind, depth, seed) * count, (args, row)
    if fault is not None:
        assert row["checksum"] == 0, (args, row)
    if cancel:
        if row["cancelled"] == count:
            assert row["checksum"] == 0, (args, row)
        else:
            assert row["checksum"] == reference(kind, depth, seed) * (count - row["cancelled"]), (args, row)
    if cancel and ((kind in KINDS[:3] and depth >= 24) or depth > 1000):
        assert row["cancelled"] == count, (args, row)
    return dict(kind=kind, seed=seed, cancel=cancel, owned=owned, borrow_register=borrow, **row)


def main():
    output = pathlib.Path(sys.argv[1])
    assert not output.exists()
    data = dict(schema=1, platform=sys.platform, revision=probe.run(
        ["git", "rev-parse", "HEAD"], capture_output=True).stdout.strip(),
        numerical=[], cancellation=[], races=[], allocation_failures=[], register_callbacks=[], pairs=[])
    for kind in KINDS:
        print("Building", kind, flush=True)
        probe.build(kind)
        for mode in probe.MODES:
            for depth in (0, 1, 3, 9, 14) if kind in KINDS[:3] else (0, 1, 3, 100):
                for seed in (1, 17, 31):
                    data["numerical"].append(invoke(kind, mode, depth, 1, seed=seed))
            if mode != "floor":
                depth = 24 if kind in KINDS[:3] else 100000000
                data["cancellation"].append(invoke(kind, mode, depth, 200, True, True))
                data["races"].append(invoke(kind, mode, 9 if kind in KINDS[:3] else 100,
                                            100, True, True))
            if mode not in ("floor", "current"):
                data["register_callbacks"].append(invoke(kind, mode, 9 if kind in KINDS[:3] else 100, 1, borrow=True))
                data["register_callbacks"].append(invoke(kind, mode, 24 if kind in KINDS[:3] else 100000000,
                                                          3, True, True, borrow=True))
            if kind == "owned":
                for fault in (1, 2, 5, 20):
                    data["allocation_failures"].append(invoke(kind, mode, 100, 1, fault=fault))
        output.with_suffix(".partial.json").write_text(json.dumps(data, indent=2) + "\n")
    # Adjacent ABBA/BAAB fresh processes, with native warmups inside each process.
    # The floor uses the same heap-based harness, but no cancellation boundary.
    rng = random.Random(20261025)
    cells = [(kind, depth, mode) for kind in KINDS[:3] for depth in (10, 14)
             for mode in probe.MODES if mode != "floor"]
    counts = {}
    for kind in KINDS[:3]:
        for depth in (10, 14):
            sample = invoke(kind, "floor", depth, 100)
            counts[kind, depth] = min(20000, max(10, int(25000000 / (sample["ns"] / 100))))
    for block in range(3):
        rng.shuffle(cells)
        for kind, depth, mode in cells:
            sequence = ("floor", mode, mode, "floor") if block % 2 == 0 else (mode, "floor", "floor", mode)
            rows = [invoke(kind, version, depth, counts[kind, depth]) for version in sequence]
            before = statistics.mean(r["ns"] / r["repetitions"] for r in rows if r["mode"] == "floor")
            after = statistics.mean(r["ns"] / r["repetitions"] for r in rows if r["mode"] == mode)
            data["pairs"].append(dict(kind=kind, depth=depth, mode=mode, block=block,
                                       floor_ns=before, candidate_ns=after, ratio=after / before,
                                       observations=rows))
        print("Paired throughput block", block + 1, "complete", flush=True)
    data["commands"] = ["python3 native/experiments/cancellation/qualify.py OUTPUT.json"]
    data["build_contract"] = "Compiler: unchanged Kalandra -O2; LLVM preparation pass then Clang -O2; optimized runtime; reserve x28 for candidates; asynchronous unwind metadata; linked native PC extents verified in two passes. floor/current retain original generated IR."
    data["hashes"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [ROOT / "build/native/native/kalandra",
                  ROOT / "build/native/native/libkalandra-task-core-optimized.a",
                  *pathlib.Path(__file__).parent.glob("*"),
                  *probe.OUT.glob("*.original.ll"), *probe.OUT.glob("*.extents"),
                  *[probe.OUT / f"{kind}.{mode}" for kind in KINDS for mode in probe.MODES]]
        if p.is_file()}
    data["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    data["protocol"] = "3 adjacent ABBA/BAAB blocks; 25ms target batches; 20 native warmup tasks per process; per-task setup and cleanup included; exact reference checks on every numerical and timing sample"
    output.write_text(json.dumps(data, indent=2) + "\n")
    print(output, flush=True)


if __name__ == "__main__":
    main()
