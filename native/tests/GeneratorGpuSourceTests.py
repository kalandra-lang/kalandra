"""Verify lazy host generators in real mixed CPU/GPU programs."""
import argparse
import os
from pathlib import Path
import re
import subprocess
from GeneratorCases import prefix, cases

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--target", choices=("metal", "vulkan", "cuda"), required=True)
parser.add_argument(
    "--variant", choices=("debug", "optimized", "sanitized", "thread-sanitized"), required=True
)
parser.add_argument("--case", action="append", dest="selected")
parser.add_argument("--compile-only", action="store_true")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
flag = {
    "debug": "-O0",
    "optimized": "-O2",
    "sanitized": "--sanitize",
    "thread-sanitized": "--thread-sanitize",
}[args.variant]
gpu_cases = dict(cases)
gpu_cases["device-await-yields"] = (
    """fn kernel(value:i64)->i64:
  return value+1
fn values()->Iterator[i64,TaskError]:
  return for i in range(3):
    value=try parallel:
      gpu=devices.gpu()
      try await spawn kernel(i) on gpu
    yield value
fn main()->i64:
  total=0
  result=for value in values():
    total=total+value
  else:total
  return result catch fn(error):return -1
""",
    6,
    "",
)
unknown = set(args.selected or ()) - gpu_cases.keys()
if unknown:
    parser.error("unknown generator cases: " + ", ".join(sorted(unknown)))
for name, (source, expected, output) in gpu_cases.items():
    if args.selected and name not in args.selected:
        continue
    directory = args.output / name
    directory.mkdir(exist_ok=True)
    source = source.replace("fn main()->i64:", "fn answer()->i64:")
    # Exercise a real device submission even for a wholly CPU generator body.
    # The generator itself remains a host continuation, not a device kernel.
    text = (
        prefix
        + source
        + """\nfrom "std/io" import println as report,IoError as ReportError
from "std/fmt" import format
fn marker()->i64:
  return 41
fn main()->unit ! (ReportError | TaskError):
  ready=try parallel:
    gpu=devices.gpu()
    try await spawn marker() on gpu
  value=answer()
  try report(format("{}:{}",ready,value))
  return ()
"""
    )
    path = directory / "source.k"
    path.write_text(text)
    bundle = directory / "bundle"
    built = subprocess.run(
        [str(args.compiler), str(path), "--target", args.target, flag, "-o", str(bundle)],
        capture_output=True,
        timeout=240,
    )
    (directory / "build.log").write_bytes(built.stdout + built.stderr)
    assert built.returncode == 0, (name, built)
    if args.compile_only:
        print("PASS compiled", name, flush=True)
        continue
    binary = bundle / ("program.exe" if os.name == "nt" else "program")
    for workers in (1, 4):
        run = subprocess.run(
            [str(binary.resolve())],
            env=dict(os.environ, KALANDRA_WORKERS=str(workers)),
            capture_output=True,
            timeout=10 if "prefetch" in name else 60,
        )
        (directory / f"run-{workers}.log").write_bytes(run.stdout + run.stderr)
        stderr = run.stderr
        if args.target == "metal":
            stderr = b"".join(
                line
                for line in stderr.splitlines(keepends=True)
                if not re.fullmatch(
                    rb"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?",
                    line,
                )
            )
        assert (
            run.returncode == 0 and run.stdout == f"{output}41:{expected}\n".encode() and not stderr
        ), (name, workers, run)
    print("PASS executed", name, flush=True)
