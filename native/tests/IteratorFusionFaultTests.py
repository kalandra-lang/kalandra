"""Fused traversal must preserve argument evaluation and arithmetic fault sites."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--compiler", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

cases = {
    "region-unused-argument": """fn before()->i64:
  value=0
  return value
fn ignored(value:i64)->i64:
  return 7
fn run(zero:i64)->i64:
  unused=before()
  values=for i in range(3):
    yield ignored(10/zero)
  total=0
  for value in values:
    total=total+value
  return total
fn main()->i64:
  return run(0)
""",
    "unused-callback-argument": """fn ignored(value:i64)->i64:
  return 7
fn run(zero:i64)->i64:
  values=for i in range(3):
    yield ignored(10/zero)
  total=0
  for value in values:
    total=total+value
  return total
fn main()->i64:
  return run(0)
""",
    "left-to-right-callback-arguments": """fn second(a:i64,b:i64)->i64:
  return b
fn run(zero:i64)->i64:
  values=for i in range(3):
    yield second(10/zero,20/zero)
  total=0
  for value in values:
    total=total+value
  return total
fn main()->i64:
  return run(0)
""",
    "unused-factory-argument-before-take-zero": """from "std/iter" import Iterator,take
fn values(ignored:i64)->Iterator[i64,never]:
  return for i in range(3):
    yield i
fn run(zero:i64)->i64:
  cursor=values(10/zero)
  limited=take(cursor,0)
  total=0
  for value in limited:
    total=total+value
  return total
fn main()->i64:
  return run(0)
""",
    "unused-consumer-argument": """fn ignored(value:i64)->i64:
  return 7
fn run(zero:i64)->i64:
  values=for i in range(3):
    yield i
  total=0
  for value in values:
    total=ignored(10/zero)
  return total
fn main()->i64:
  return run(0)
""",
}

for name, text in cases.items():
    directory = args.output / name
    directory.mkdir(exist_ok=True)
    source = directory / "main.k"
    source.write_text(text)
    binary = directory / "program.exe"
    compiled = subprocess.run(
        [
            str(args.compiler),
            str(source),
            "-O2",
            "--test-entry",
            "main",
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
        timeout=180,
    )
    (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, (name, compiled)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    line, column = next(
        (n, line.index("10/zero") + 1)
        for n, line in enumerate(text.splitlines(), 1)
        if "10/zero" in line
    )
    (directory / "execution.log").write_text(result.stdout + result.stderr)
    assert result.returncode == 86 and not result.stdout, (name, result)
    assert result.stderr == f"ArithmeticFault at source {line}:{column}\n", (
        name,
        result,
    )
    print("PASS", name)
