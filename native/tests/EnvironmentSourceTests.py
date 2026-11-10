"""Inherited configuration through the public CPU and mixed GPU application entry."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from EnvironmentCases import SOURCE, VARIABLES, expected, with_environment

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

IMPORTS = '''from "std/env" import get, EnvValue, EnvError
from "std/io" import println, IoError
from "std/fmt" import format
'''


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


def run(name, source, stdout, variables=VARIABLES):
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
            env = with_environment(dict(os.environ, KALANDRA_WORKERS=workers), variables,
                                   windows=windows)
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


run("configuration", SOURCE, expected(windows))
run("returned-values", IMPORTS + '''fn read(name:String)->EnvValue ! EnvError:
  return get(name)

fn describe(setting:EnvValue)->String:
  return match setting:
    EnvValue.Missing: "missing"
    EnvValue.Value {value}: format("text={}",value)

fn main()->unit ! (EnvError | TaskError | IoError):
  result=try parallel:
    first=spawn read("KALANDRA_QUAL_ENV_TEXT")
    second=spawn read("KALANDRA_QUAL_ENV_EMPTY")
    a=try await first
    b=try await second
    try println(describe(b))
    a
  try println(describe(result))
  return ()
''', "text=\ntext=Grüße=日本語\n")
run("invalid-name", IMPORTS + '''fn main()->unit ! (EnvError | IoError):
  for key in ["", "bad=name"]:
    result=get(key) catch fn(error):
      return match error:
        EnvError.InvalidName: EnvValue.Value {value:"invalid name"}
        EnvError.InvalidEncoding: EnvValue.Missing
    text=match result:
      EnvValue.Value {value}: value
      EnvValue.Missing: "missing"
    try println(text)
  return ()
''', "invalid name\ninvalid name\n")
if not windows and not args.compile_only:
    run("invalid-encoding", IMPORTS + '''fn main()->unit ! (EnvError | IoError):
  result=get("KALANDRA_QUAL_ENV_INVALID") catch fn(error):
    return match error:
      EnvError.InvalidName: EnvValue.Missing
      EnvError.InvalidEncoding: EnvValue.Value {value:"invalid encoding"}
  text=match result:
    EnvValue.Value {value}: value
    EnvValue.Missing: "missing"
  try println(text)
  return ()
''', "invalid encoding\n", {"KALANDRA_QUAL_ENV_INVALID": b"\xff"})

if gpu:
    build("device-rejection", IMPORTS + '''fn work(n:i64)->i64 ! EnvError:
  setting=try get("KALANDRA_QUAL_ENV_TEXT")
  return match setting:
    EnvValue.Missing: n
    EnvValue.Value {value}: length(value)
fn main()->unit ! (EnvError | TaskError | IoError):
  result=try parallel:
    gpu=devices.gpu()
    try await spawn work(21) on gpu
  try println(format("{}", result))
  return ()
''', b"error[G001]")
else:
    executable = build("exit-status", '''from "std/env" import get, EnvValue, EnvError
from "std/process" import ExitStatus
fn main()->ExitStatus ! EnvError:
  setting=try get("KALANDRA_QUAL_ENV_TEXT")
  matches=match setting:
    EnvValue.Missing: false
    EnvValue.Value {value}: value == "Grüße=日本語"
  if matches:
    return ExitStatus {code:42}
  return ExitStatus {code:1}
''')
    if not args.compile_only:
        result = subprocess.run([executable], capture_output=True, timeout=30,
                                env=with_environment(os.environ, windows=windows))
        assert result.returncode == 42 and not result.stdout and not result.stderr, result
    build("private-intrinsic", '''fn main()->unit:
  value=__kalandra_environment_get("key")
  return ()
''', b"error[S031]")
print("PASS: lookup rejected in unsupported context", flush=True)
