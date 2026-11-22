"""Measure four levels of one compiler, with independently checked process output."""
import argparse
import hashlib
import json
import platform
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FLAGS = ["-O0", "-O1", "-O2", "-O3"]
CONTROL = "O2 repeat"


def residues(count, divisor, power):
    cycles, tail = divmod(count, divisor)
    return cycles * sum(x**power for x in range(divisor)) + sum(x**power for x in range(tail))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def timed(argv, expected=None):
    start = time.perf_counter_ns()
    p = subprocess.run(argv, capture_output=True, check=True, timeout=180)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if expected is not None and (p.stdout.strip() != expected.encode() or p.stderr):
        raise ValueError((argv, expected, p.stdout, p.stderr))
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("report", type=Path)
    parser.add_argument("--blocks", type=int, default=9)
    parser.add_argument("--seed", type=int, default=272)
    args = parser.parse_args()
    if args.blocks < 3:
        parser.error("use at least three blocks")
    compiler = args.compiler.resolve()
    rng = random.Random(args.seed)
    count, depth, divisor, offset = 100_000_000, 22, 97, 7919
    row = offset + residues(100_000, divisor, 1)
    leaves = 2**depth
    cases = {
        "billion": ([str(divisor), str(offset)], f"{row} {row * 10_000}"),
        "recursive": ([str(depth)], str(residues(2 * leaves, 97, 1) - residues(leaves, 97, 1))),
        "numeric": ([str(count)], str(residues(count, 257, 2))),
    }
    report = dict(compiler_sha256=digest(compiler), platform=platform.platform(),
                  seed=args.seed, blocks=args.blocks, cases={})
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        executable = "program.exe" if platform.system() == "Windows" else "program"
        for name, (inputs, expected) in cases.items():
            source = ROOT / (name + ".k")
            measurements = dict(source_sha256=digest(source), inputs=inputs, expected=expected,
                                bytes={}, binary_sha256={}, builds=[], runs=[])
            report["cases"][name] = measurements
            # Use the same output basename at every level (also the Mach-O signing identity).
            binaries = {flag: work / flag / executable for flag in FLAGS}
            for flag, binary in binaries.items():
                binary.parent.mkdir(exist_ok=True)
                timed([compiler, source, flag, "-o", binary])
                measurements["bytes"][flag] = binary.stat().st_size
                measurements["binary_sha256"][flag] = digest(binary)
                timed([binary, *inputs], expected)
            binaries[CONTROL] = binaries["-O2"]
            output = work / "compile" / executable
            output.parent.mkdir(exist_ok=True)
            for block in range(args.blocks):
                for phase in ("builds", "runs"):
                    order = list(binaries)
                    rng.shuffle(order)
                    results = {}
                    for label in order:
                        if phase == "builds":
                            flag = "-O2" if label == CONTROL else label
                            results[label] = timed([compiler, source, flag, "-o", output])
                        else:
                            results[label] = timed([binaries[label], *inputs], expected)
                    measurements[phase].append(results)
            print(name, {phase: {label: round(statistics.median(block[label] for block in measurements[phase]), 2)
                                 for label in binaries} for phase in ("builds", "runs")}, flush=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n")
    if digest(compiler) != report["compiler_sha256"]:
        raise RuntimeError("compiler changed during measurement")


if __name__ == "__main__":
    main()
