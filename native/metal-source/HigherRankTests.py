"""Compile and optionally run higher-rank numerical source programs."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--variant", choices=("debug", "optimized", "sanitized", "thread-sanitized"))
args = parser.parse_args()
args.root.mkdir(parents=True, exist_ok=True)
flags = [] if not args.variant else ["--target", "metal", {
    "debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize",
    "thread-sanitized": "--thread-sanitize"}[args.variant]]

def run(command, expected=0, output=None, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90, env=env)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    assert not any(t in result.stderr for t in ("AddressSanitizer", "ThreadSanitizer", "runtime error:")), result.stderr
    if output is not None:
        assert result.stdout == output, (command, result.stdout, output)
    return result

cases = 0
for rank in range(3, 9):
    dynamic = "Tensor[float32," + ",".join(["?"] * rank) + "]"
    fixed = "Tensor[float32," + ",".join(["1"] * (rank - 1) + ["3"]) + "]"
    data = "[1,2,3]"
    for _ in range(rank - 1):
        data = "[" + data + "]"
    for name, body, scalar, expected in (
        ("map", "values + 1", False, "[40000000,40400000,40800000]\n"),
        ("sum", "reduceSum(values)", True, "40c00000\n"),
        ("feedback", "values / reduceSum(values)", False, "[3e2aaaab,3eaaaaab,3f000000]\n"),
        ("multiple", "reduceSum(values) + reduceSum(values + 1)", True, "41700000\n"),
        ("empty-map", "values + 1", False, "[]\n"),
        ("empty-sum", "reduceSum(values)", True, "00000000\n"),
        ("empty-feedback", "values / reduceSum(values)", False, "[]\n"),
        ("choice", "if reduceSum(values) > 0:\n    values + 1\n  else:\n    values - 1", False, "[40000000,40400000,40800000]\n"),
    ):
        result = "float32" if scalar else dynamic
        literal, dimensions = data, fixed
        if name.startswith("empty-"):
            literal = "[]"
            for _ in range(rank - 1):
                literal = "[" + literal + "]"
            dimensions = fixed.replace(",3]", ",0]")
        source = (f"fn operation(values:{dynamic})->{result}:\n  {body}\n"
                  f"fn main()->{result} ! TaskError:\n  try parallel:\n"
                  "    gpu=devices.gpu()\n"
                  f"    values:{dimensions}=tensor({literal})\n"
                  "    resident=try await transfer(values,gpu)\n"
                  "    work=spawn operation(resident) on gpu\n"
                  "    result=try await work\n"
                  "    cached=try await work\n" +
                  ("    cached\n" if scalar else "    try await transfer(cached,devices.cpu())\n"))
        identifier = f"rank{rank}-{name}"
        path, destination = args.root / (identifier + ".k"), args.root / identifier
        path.write_text(source)
        run([args.compiler,"--arithmetic","software", path, *flags, "-o", destination])
        if args.variant:
            for workers in (1, 2, 4):
                run([destination / "program"], output=expected,
                    env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
        cases += 1
    # Equal element counts do not establish shape equality on higher axes.
    other = "[[1],[2],[3]]"
    for _ in range(rank - 2):
        other = "[" + other + "]"
    other_type = "Tensor[float32," + ",".join(["1"] * (rank - 2) + ["3", "1"]) + "]"
    source = (f"fn identity(values:{dynamic})->{dynamic}:\n  values\n"
              f"fn add(a:{dynamic},b:{dynamic})->{dynamic}:\n  a+b\n"
              f"fn main()->{dynamic} ! TaskError:\n  try parallel:\n"
              "    gpu=devices.gpu()\n"
              f"    a:{fixed}=tensor({data})\n"
              f"    b:{other_type}=tensor({other})\n"
              "    x=try await transfer(identity(a),gpu)\n"
              "    y=try await transfer(identity(b),gpu)\n"
              "    result=try await spawn add(x,y) on gpu\n"
              "    try await transfer(result,devices.cpu())\n")
    path, destination = args.root / f"rank{rank}-shape.k", args.root / f"rank{rank}-shape"
    path.write_text(source)
    run([args.compiler,"--arithmetic","software", path, *flags, "-o", destination])
    if args.variant:
        for workers in (1, 2, 4):
            failure = run([destination / "program"], expected=1, output="",
                          env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
            assert "Metal task failed: 1/1" in failure.stderr, failure.stderr
    cases += 1
print(f"Higher-rank numerical integration: {cases} programs, ranks 3..8, maps/sums/resident feedback/multiple reductions/choices/empty shapes/extent faults")
