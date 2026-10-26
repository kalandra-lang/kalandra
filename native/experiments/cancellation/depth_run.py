#!/usr/bin/env python3
"""Scaling beyond depth 14, plus linear-work genuinely non-tail deep recursion."""
import datetime
import functools
import hashlib
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
from fractions import Fraction
import run as preparation

ROOT = preparation.ROOT
HERE = pathlib.Path(__file__).parent.resolve()
OUT = ROOT / "benchmarks/.build/cancellation-depth"
sys.path.insert(0, str(ROOT / "native/benchmarks/fair"))
import protocol as p

NAMES = ("polynomial", "branches", "euclid", "chain")
DEPTHS = (14, 16, 18, 20, 22)
CHAIN_DEPTHS = (100, 1000, 10000)


@functools.cache
def exact(kind, depth, seed):
    lo, hi = seed * (1 << depth), (seed + 1) * (1 << depth)
    if kind == 0:
        square = lambda n: n * (n + 1) * (2 * n + 1) // 6
        linear = (lo + hi - 1) * (hi - lo) // 2
        return Fraction(square(hi - 1) - square(lo - 1) + 2 * linear - 4 * (hi - lo), 8)
    if kind == 1:
        result = Fraction(0)
        for residue, factor, offset in [(0, Fraction(1, 2), 0),
                                        (1, Fraction(-1, 4), 0), (2, 1, Fraction(1, 8))]:
            first = lo + (residue - lo) % 3
            count = max(0, (hi - 1 - first) // 3 + 1)
            total = count * (2 * first + 3 * (count - 1)) // 2
            result += factor * total + offset * count
        return result
    if kind == 2:
        # gcd(13*x+7,7*x+11) divides 94. Count its factors independently.
        count = lambda modulus, residue: ((hi - 1 - residue) // modulus -
                                          (lo - 1 - residue) // modulus)
        return Fraction(hi - lo + count(2, 1) + 46 * count(47, 32) + 46 * count(94, 79))
    history = []
    x = seed
    for _ in range(depth):
        history.append(x)
        x = (x * 13 + 7) % 1000003
    result = Fraction(x)
    for x in reversed(history):
        result = result / 2 + Fraction(x, 4)
    return result


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    flags = p.run([preparation.LLVM / "llvm-config", "--cxxflags", "--ldflags", "--libs",
                   "core", "irreader", "analysis", "--system-libs"]).split()
    p.run([p.CLANG, HERE / "Prepare.cpp", "-o", OUT / "prepare", *flags,
           "-std=c++20", "-fexceptions"])
    probe = (HERE / "Probe.cpp").read_text()
    assert probe.count("warmup != 20") == 1
    (OUT / "DepthProbe.cpp").write_text(probe.replace("warmup != 20", "warmup != 2"))
    p.run([p.CLANG, "-std=c++20", "-O2", "-ffixed-x28", "-I", HERE, "-c",
           OUT / "DepthProbe.cpp", "-o", OUT / "probe.o"])
    p.run([p.CLANG, "-c", HERE / "Enter.S", "-o", OUT / "enter.o"])
    for name in NAMES:
        source = HERE / "Depth.k" if name == "chain" else ROOT / "native/benchmarks/fair/source.k"
        original = OUT / f"{name}.original.ll"
        p.run([ROOT / "build/native/native/kalandra", source, "--test-entry", name,
               "--emit-llvm", original, "-O2", "-o", OUT / f"{name}.standalone"])
        for mode in ("floor", "current", "hybrid"):
            ir, binary = OUT / f"{name}.{mode}.ll", OUT / f"{name}.{mode}"
            def transform(extra=()):
                p.run([OUT / "prepare", original, ir, mode, *extra])
                p.run([p.CLANG, "-O2", ir, OUT / "probe.o", OUT / "enter.o",
                       ROOT / "build/native/native/libkalandra-task-core-optimized.a",
                       "-lpthread", "-o", binary])
            transform()
            if mode == "hybrid":
                spans = preparation.extents(binary)
                manifest = OUT / f"{name}.extents"
                manifest.write_text("".join(f"{key} {size}\n" for key, size in spans.items()))
                transform([manifest])
                assert preparation.extents(binary) == spans
    build_references()


def build_references():
    OUT.mkdir(parents=True, exist_ok=True)
    driver = (ROOT / "native/benchmarks/fair/SourceDriver.cpp").read_text()
    assert driver.count("kind > 2") == driver.count("depth > 14") == 1
    driver = driver.replace("kind > 2", "kind > 3").replace("depth > 14", "depth > (kind == 3 ? 10000 : 22)")
    (OUT / "DepthDriver.cpp").write_text(driver.replace('#include "../../runtime/Runtime.h"\n', ''))
    p.run([p.CLANG, "-std=c++23", "-O2", "-ffp-contract=off", "-c", HERE / "Depth.cpp",
           "-o", OUT / "cpp.o"])
    p.run([p.CLANG, "-std=c++23", "-O2", OUT / "DepthDriver.cpp", OUT / "cpp.o", "-o", OUT / "cpp"])
    rust = (ROOT / "native/benchmarks/fair/source.rs").read_text()
    rust = rust.replace("_ => euclid(x, d),", "2 => euclid(x, d),\n        _ => chain(x, d),")
    rust = rust.replace("(0..=2).contains(&kind)", "(0..=3).contains(&kind)").replace(
        "(0..=14).contains(&depth)", "(0..=if kind == 3 { 10000 } else { 22 }).contains(&depth)")
    rust += "\nfn chain(x:i64,d:i64)->f64 { if d==0 { x as f64 } else { let next=(x*13+7)%1000003; let child=chain(next,d-1); child*0.5+(x as f64)*0.25 } }\n"
    (OUT / "depth.rs").write_text(rust)
    p.run([p.RUST, "--edition=2021", "-C", "opt-level=2", "-C", "lto=off",
           OUT / "depth.rs", "-o", OUT / "rust"])


def observe(language, kind, depth, count, rounds=1, seed=17):
    expected = float(exact(kind, depth, seed))
    if language in ("cpp", "rust"):
        output = p.run([OUT / language, kind, depth, count, rounds, seed])
        rows = [json.loads(line) for line in output.splitlines()]
    else:
        rows = []
        for round in range(-2, rounds):
            command = [OUT / f"{NAMES[kind]}.{language}", language, depth, count, 0, 0]
            p.COMMANDS.append(list(map(str, command)))
            result = subprocess.run(list(map(str, command)), text=True, capture_output=True,
                                    check=True, timeout=120,
                                    env=dict(os.environ, KALANDRA_PROBE_SEED=str(seed)))
            assert not result.stderr, result.stderr
            row = json.loads(result.stdout)
            rows.append(dict(round=round, ns=row["ns"], checksum=row["checksum"]))
    # Single-result qualification allows at most two ULP from the exact rational
    # sum. Timed batches include ordinary repeated-sum rounding explicitly.
    eps = 2.0 ** -53
    allowed = (count * 2 * math.ulp(expected) +
               ((count - 1) * eps / (1 - (count - 1) * eps)) * count * abs(expected) +
               math.ulp(expected * count))
    for row in rows:
        assert math.isfinite(row["checksum"])
        if count == 1:
            assert abs(row["checksum"] - expected) <= (2 if kind in (0, 3) else 0) * math.ulp(expected), (language, kind, depth, seed, row, expected)
        else:
            assert abs(row["checksum"] - expected * count) <= allowed, (language, kind, depth, count, row, expected, allowed)
    return rows


def main():
    output = pathlib.Path(sys.argv[1])
    assert not output.exists()
    # Exhaustively check the independently derived constant-time tree references
    # against direct Fraction/integer/GCD sums on tractable domains.
    for depth in range(10):
        for seed in (1, 17, 31):
            lo, hi = seed * (1 << depth), (seed + 1) * (1 << depth)
            assert exact(1, depth, seed) == sum((Fraction(x, 2) if x % 3 == 0 else
                 Fraction(-x, 4) if x % 3 == 1 else Fraction(x) + Fraction(1, 8)) for x in range(lo, hi))
            assert exact(2, depth, seed) == sum(math.gcd(13*x+7, 7*x+11) for x in range(lo, hi))
    build()
    overflows = []
    for name in NAMES[:3]:
        for mode in ("floor", "current", "hybrid"):
            command = [OUT / f"{name}.{mode}", mode, 100, 1, 0, 0, 0, 4]
            row = json.loads(p.run(command))
            assert row["checksum"] == 0
            overflows.append(dict(kind=name, expected_status=4, result=row))
    checks = []
    for kind in range(4):
        depths = (0, 1, *DEPTHS) if kind != 3 else (0, 1, *CHAIN_DEPTHS)
        for depth in depths:
            for seed in (1, 17, 31):
                for language in ("cpp", "rust", "floor", "current", "hybrid"):
                    rows = observe(language, kind, depth, 1, seed=seed)
                    checks.append(dict(language=language, kind=NAMES[kind], depth=depth, seed=seed,
                        expected=float(exact(kind, depth, seed)), samples=rows))
        print("Verified", NAMES[kind], flush=True)
    cells = [[version, language, kind, depth] for version in ("baseline", "current")
             for language in ("cpp", "rust") for kind in range(4)
             for depth in (CHAIN_DEPTHS if kind == 3 else DEPTHS)]
    def invoke(cell, count, rounds):
        version, language, kind, depth = cell
        return observe(language if version == "baseline" else "hybrid", kind, depth, count, rounds)
    result = p.measure_paired(cells, invoke, lambda cell: float(exact(cell[2], cell[3], 17)),
                              blocks=3, rounds=1, target_ms=20)
    result.update(schema=1, timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        verification=checks, depth_100_tree_overflow=overflows, contract="Original tree algorithms extended to depths 14/16/18/20/22; linear-work non-tail chain at 100/1000/10000. Same algorithm per language. Experimental hybrid Kalandra with cancellation available vs ordinary optimized C++/Rust. Independent rational references; <=2 ULP single-result error; timed checksum bounds explicitly include repeated-sum rounding. No depth-100 binary tree was executed.",
        revision=p.run(["git","rev-parse","HEAD"]).strip(), commands=p.COMMANDS,
        hashes={p.key(path):p.sha(path) for path in [HERE/"Depth.k", HERE/"Depth.cpp", pathlib.Path(__file__),
          HERE/"Probe.cpp", HERE/"Prepare.cpp", HERE/"Enter.S", HERE/"Control.h",
          ROOT/"native/benchmarks/fair/source.k",ROOT/"native/benchmarks/fair/Source.cpp",
          ROOT/"native/benchmarks/fair/source.rs",ROOT/"build/native/native/kalandra",
          ROOT/"build/native/native/libkalandra-task-core-optimized.a", *OUT.glob("*")] if path.is_file()})
    output.write_text(json.dumps(result,indent=2)+"\n")
    for kind in range(4):
        for depth in (CHAIN_DEPTHS if kind==3 else DEPTHS):
            ratios=[statistics.median(1/r["speedup"] for r in result["pairs"] if r["group"]==[lang,kind,depth]) for lang in ("cpp","rust")]
            print(NAMES[kind],depth,"hybrid / cpp,rust",*ratios,flush=True)


if __name__ == "__main__":
    main()
