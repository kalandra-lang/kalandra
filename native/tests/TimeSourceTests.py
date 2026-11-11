"""Monotonic clocks and cancellable sleep through public CPU and GPU host entries."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from TimeCases import SOURCE, EXPECTED

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--variant", choices=("debug", "optimized", "sanitized", "thread-sanitized"),
                    required=True)
parser.add_argument("--target", choices=("cpu", "metal", "vulkan", "cuda"), default="cpu")
parser.add_argument("--compile-only", action="store_true")
args = parser.parse_args()
compiler, output = args.compiler.resolve(), args.output.resolve()
windows = os.name == "nt"
gpu = args.target != "cpu"
flag = {"debug": "-O0", "optimized": "-O2", "sanitized": "--sanitize",
        "thread-sanitized": "--thread-sanitize"}[args.variant]

def build(name, source, diagnostic=None):
    root = output / name
    root.mkdir(parents=True, exist_ok=True)
    path = root / "main.k"
    path.write_text(source, encoding="utf-8")
    executable = root / ("program.exe" if windows else "program")
    destination = root / "app" if gpu else executable
    command = [compiler, path, flag, "-o", destination]
    if gpu:
        command.extend(("--target", args.target))
    built = subprocess.run(command, capture_output=True, timeout=240)
    (root / "build.log").write_bytes(built.stdout + built.stderr)
    if diagnostic:
        assert built.returncode and diagnostic in built.stderr, (name, built)
        return None
    assert built.returncode == 0, (name, built)
    return destination / executable.name if gpu else executable


def run(name, source, stdout):
    if gpu:
        source = re.sub(r"(fn main\(\)\s*->\s*unit\s*!\s*\()([^)]+)(\):)",
                        lambda m: m[0] if "TaskError" in m[2] else
                        m[1] + m[2] + " | TaskError" + m[3], source)
        source = re.sub(r"(fn main\([^\n]+\n)", r"\1  numberFromGpu=try parallel:\n"
                        r"    gpu=devices.gpu()\n    try await spawn double(21) on gpu\n"
                        r'  try println(format("{}",numberFromGpu))' "\n", source)
        source = "fn double(value:i64)->i64:\n  return value*2\n" + source
        stdout = "42\n" + stdout
    executable = build(name, source)
    if not args.compile_only:
        for workers in ("1", "4"):
            env = dict(os.environ, KALANDRA_WORKERS=workers)
            result = subprocess.run([executable], cwd=executable.parent, capture_output=True,
                                    env=env, timeout=90)
            (executable.parent / f"run-{workers}.log").write_bytes(result.stdout + result.stderr)
            stderr = result.stderr
            if args.target == "metal":
                stderr = b"".join(line for line in stderr.splitlines(keepends=True)
                                  if not re.fullmatch(rb"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ "
                                                     rb"program\[\d+:\d+\] Metal API Validation Enabled\r?\n?", line))
            assert result.returncode == 0 and result.stdout == stdout.encode() and not stderr, (
                name, workers, result)
    print("PASS:", "compiled" if args.compile_only else "executed", name, flush=True)


run("time", SOURCE, EXPECTED)
build("opaque-constructor", '''from "std/time" import Duration
fn main()->unit:
  value=Duration {__nanoseconds:u64(1)}
  return ()
''', b"error[S081]")
build("opaque-field", '''from "std/time" import nanoseconds
fn main()->unit:
  value=nanoseconds(u64(1))
  number=value.__nanoseconds
  return ()
''', b"error[S081]")
build("wrong-unit", '''from "std/time" import sleep, TimeError
fn main()->unit ! TimeError:
  try sleep(u64(1))
  return ()
''', b"error[")
if gpu:
    build("device-rejection", '''from "std/time" import now, TimeError
fn clock()->u64 ! TimeError:
  start=try now()
  return u64(0)
fn main()->unit ! (TimeError | TaskError):
  result=try parallel:
    gpu=devices.gpu()
    try await spawn clock() on gpu
  return ()
''', b"error[G001]")

build("opaque-update", '''from "std/time" import nanoseconds
fn main()->unit:
  value=nanoseconds(u64(1))
  changed=value with {__nanoseconds:u64(2)}
  return ()
''', b"error[S081]")
build("private-intrinsic", '''fn main()->unit:
  value=__kalandra_time_now()
  return ()
''', b"error[S031]")
