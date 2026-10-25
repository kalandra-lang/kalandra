#!/usr/bin/env python3
"""Causal diagnostic of prototype harness entry cost; no kernel/safety edits."""
import datetime
import json
import math
import os
import pathlib
import random
import statistics
import subprocess
import sys
import depth_run as d
import run as prep

OUT = d.ROOT / "benchmarks/.build/cancellation-entry"
VARIANTS = ("heap", "stack", "heap-no-clock", "stack-no-clock", "stack-clean")


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    base = (d.OUT / "DepthProbe.cpp").read_text()
    assert "warmup != 2" in base
    stack = '''
template <class T> struct StackState {
  T value{};
  T *get() noexcept { return &value; }
  T *operator->() noexcept { return &value; }
};
'''
    for variant in VARIANTS:
        text = base.replace("warmup != 2", "warmup != 2 * repetitions")
        if variant.startswith("stack"):
            text = text.replace("using kalandra::experiment::Control;",
                                "using kalandra::experiment::Control;" + stack)
            for kind in ("Control", "kalandra::runtime::Invocation"):
                assert text.count(f"std::make_unique<{kind}>()") == 2
                text = text.replace(f"std::make_unique<{kind}>()", f"StackState<{kind}>{{}}")
        if variant in ("heap-no-clock", "stack-no-clock", "stack-clean"):
            begin = text.index("  control->finishedNs.store(")
            end = text.index(";", begin) + 1
            text = text[:begin] + text[end:]
        if variant == "stack-clean":
            # Hybrid does not use the guard-fusion experiment's poisonable limit.
            # The real source-run boundary still caches/checks physical stack bounds.
            begin = text.index("  kalandra::runtime::InvocationABI::cacheStackBounds(invocation);")
            end = text.index("  control->active.store", begin)
            text = text[:begin] + text[end:]
        source = OUT / f"{variant}.cpp"
        source.write_text(text)
        d.p.run([d.p.CLANG, "-std=c++20", "-O2", "-ffixed-x28", "-I", d.HERE,
                 "-c", source, "-o", OUT / f"{variant}.o"])
        d.p.run([d.p.CLANG, "-O2", d.OUT / "chain.hybrid.ll", OUT / f"{variant}.o",
                 d.OUT / "enter.o", d.ROOT / "build/native/native/libkalandra-task-core-optimized.a",
                 "-lpthread", "-o", OUT / variant])
        assert prep.extents(OUT / variant) == prep.extents(d.OUT / "chain.hybrid")


def observe(version, depth, count, seed=17):
    expected = float(d.exact(3, depth, seed))
    if version in ("cpp", "rust"):
        text = d.p.run([d.OUT / version, 3, depth, count, 1, seed])
        rows = [json.loads(line) for line in text.splitlines()]
        row = next(row for row in rows if row["round"] == 0)
    else:
        args = [OUT / version, "hybrid", depth, count, 0, 0]
        d.p.COMMANDS.append(list(map(str, args)))
        result = subprocess.run(list(map(str, args)), text=True, capture_output=True, check=True,
                                env=dict(os.environ, KALANDRA_PROBE_SEED=str(seed)))
        assert not result.stderr, result.stderr
        row = json.loads(result.stdout)
    eps = 2**-53
    bound = (count * 2 * math.ulp(expected) +
             ((count-1)*eps/(1-(count-1)*eps))*count*abs(expected) + math.ulp(expected*count))
    assert abs(row["checksum"]-count*expected) <= bound, (version, depth, row, expected)
    return {**row, "version": version, "depth": depth, "count": count}


def main():
    output = pathlib.Path(sys.argv[1])
    assert not output.exists()
    build()
    checks = [observe(version, depth, 1, seed) for version in VARIANTS
              for depth in (0, 1, 100, 1000, 10000) for seed in (1, 17, 31)]
    # Semantic cancellation checks use the original tail workload, not a growing
    # chain that could exhaust physical stack before cancellation. Clockless
    # variants are checked for cancellation/cleanup, never latency statistics.
    cancellation = []
    tail = d.ROOT / "benchmarks/.build/cancellation/tail.hybrid.ll"
    for variant in VARIANTS:
        binary = OUT / f"{variant}.tail"
        d.p.run([d.p.CLANG, "-O2", tail, OUT / f"{variant}.o", d.OUT / "enter.o",
                 d.ROOT / "build/native/native/libkalandra-task-core-optimized.a", "-lpthread", "-o", binary])
        result = json.loads(d.p.run([binary, "hybrid", 100000000, 100, 1, 1]))
        assert result["cancelled"] == 100 and result["checksum"] == 0
        cancellation.append(dict(variant=variant, result=result))
    records = []
    pairs = []
    rng = random.Random(20261025)
    groups = [(version, reference, depth) for version in VARIANTS
              for reference in ("cpp", "rust") for depth in (100, 1000)]
    for block in range(5):
        rng.shuffle(groups)
        for version, reference, depth in groups:
            sequence = (reference, version, version, reference) if block % 2 == 0 else (version, reference, reference, version)
            count = 20000 if depth == 100 else 2000
            rows = [observe(name, depth, count) for name in sequence]
            before = statistics.mean(row["ns"]/count for row in rows if row["version"] == reference)
            after = statistics.mean(row["ns"]/count for row in rows if row["version"] == version)
            pairs.append(dict(version=version, reference=reference, depth=depth, block=block,
                              reference_ns=before, candidate_ns=after, ratio=after/before,
                              samples=rows))
        print("Ablation paired block", block+1, "complete", flush=True)
    cells = [(version, 0) for version in (*VARIANTS, "cpp", "rust")]
    for trial in range(7):
        rng.shuffle(cells)
        records.extend(dict(trial=trial, **observe(version, depth, 100000)) for version, depth in cells)
    result = dict(schema=1, timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        contract="Diagnostic harness ablation, unchanged hybrid kernel IR and runtime. Five paired ABBA/BAAB blocks per candidate/reference/depth, same iterations. stack moves only experiment state to caller stack; no-clock removes per-invocation latency-only timestamp; clean also removes guard-experiment-only duplicate stack-bound cache setup. Real physical stack checks, checked arithmetic, cancellation registration/cleanup, status and ledger checks remain. All versions execute two full-batch warmups before timing. Primary old benchmark scores are retained separately.",
        verification=checks, cancellation=cancellation, zero_depth=records, pairs=pairs,
        commands=d.p.COMMANDS, hashes={d.p.key(path):d.p.sha(path) for path in [
          pathlib.Path(__file__),d.HERE/"Probe.cpp",d.HERE/"Control.h",d.HERE/"Enter.S",
          d.OUT/"DepthProbe.cpp",d.OUT/"chain.hybrid.ll",d.OUT/"enter.o",
          d.OUT/"cpp",d.OUT/"rust",tail,
          d.ROOT/"build/native/native/libkalandra-task-core-optimized.a",*OUT.glob("*")] if path.is_file()})
    output.write_text(json.dumps(result,indent=2)+"\n")
    for version in VARIANTS:
        zero = statistics.median(row["ns"]/row["count"] for row in records if row["version"] == version)
        ratios = [statistics.median(row["ratio"] for row in pairs if row["version"] == version and row["reference"]==reference and row["depth"]==100) for reference in ("cpp","rust")]
        print(version,"zero_ns",zero,"depth100_vs_cpp_rust",*ratios,flush=True)


if __name__ == "__main__":
    main()
