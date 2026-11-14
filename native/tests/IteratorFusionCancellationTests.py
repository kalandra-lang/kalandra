"""Check that fused infinite producers retain bounded cancellation and cleanup."""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--compiler", type=Path, required=True)
p.add_argument("--clang", type=Path, required=True)
p.add_argument("--runtime", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument(
    "--variant",
    choices=("debug", "optimized", "sanitized", "thread-sanitized"),
    required=True,
)
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
compiler_flags = {
    "debug": ["-O0"],
    "optimized": ["-O2"],
    "sanitized": ["--sanitize"],
    "thread-sanitized": ["--thread-sanitize"],
}[args.variant]
native_flags = {
    "debug": ["-O0"],
    "optimized": ["-O2"],
    "sanitized": ["-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
    "thread-sanitized": ["-O1", "-fsanitize=thread"],
}[args.variant]

for mode, creation in {
    "source": "while true:\n    yield 1\n",
    "adapters": "mapped(filter(iter(steps(9223372036854775807)),fn(x:i64)->bool:return x%2==0),fn(x:i64)->i64:return x%97)\n",
    "region": "for i in range(9223372036854775807):\n    yield i%97\n",
    "control-source": "while true:\n    yield 1\n",
}.items():
    directory = args.output / mode
    directory.mkdir(exist_ok=True)
    source, ir, binary = (
        directory / "main.k",
        directory / "main.ll",
        directory / "program.exe",
    )
    region_prefix = (
        "fn before()->i64:\n  value=0\n  return value\n" if mode == "region" else ""
    )
    source.write_text(
        'from "std/iter" import iter,range as steps,map as mapped,filter\n'
        + region_prefix
        + "fn run()->i64:\n"
        + ("  ignored=before()\n" if mode == "region" else "")
        + "  values="
        + creation
        + "  total=0\n  for value in values:\n"
        + ("    if total<0:\n      break\n" if mode == "control-source" else "")
        + "    total=(total+value)%97\n  return total\n"
        "fn main()->i64:\n  return run()\n"
    )
    commands = [
        [
            args.compiler,
            source,
            *compiler_flags,
            "--test-entry",
            "run",
            "--emit-llvm",
            ir,
            "-o",
            directory / "standalone.exe",
        ],
        [
            args.clang,
            "-std=c++23",
            *native_flags,
            "-pthread",
            "-Wno-override-module",
            Path(__file__).with_name("IteratorFusionCancellationDriver.cpp"),
            ir,
            args.runtime,
            "-o",
            binary,
        ],
        [binary],
    ]
    environment = dict(os.environ)
    if os.name == "nt":
        resources = list(
            args.clang.resolve().parent.parent.glob("lib/clang/*/lib/windows")
        )
        environment["PATH"] = (
            os.pathsep.join(map(str, resources))
            + os.pathsep
            + environment.get("PATH", "")
        )
    for index, command in enumerate(commands):
        result = subprocess.run(
            list(map(str, command)), capture_output=True, timeout=60, env=environment
        )
        (directory / f"step-{index}.log").write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (
            mode,
            index,
            result.returncode,
            result.stdout,
            result.stderr,
        )
    print("PASS", mode, args.variant)
