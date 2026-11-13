"""Execute source generators through the public compiler and bounded runtime."""
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
    "--variant", choices=("debug", "optimized", "sanitized", "thread-sanitized"), required=True
)
p.add_argument("--case", action="append", dest="selected")
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parent
from GeneratorCases import prefix, cases

unknown = set(args.selected or ()) - cases.keys()
if unknown:
    p.error("unknown generator cases: " + ", ".join(sorted(unknown)))
environment = dict(os.environ)
if os.name == "nt":
    resources = list(args.clang.resolve().parent.parent.glob("lib/clang/*/lib/windows"))
    environment["PATH"] = (
        os.pathsep.join(map(str, resources)) + os.pathsep + environment.get("PATH", "")
    )


def run(command, directory, name, timeout=180):
    result = subprocess.run(
        list(map(str, command)), capture_output=True, timeout=timeout, env=environment
    )
    (directory / (name + ".log")).write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
    return result


for name, (source, expected, output) in cases.items():
    if args.selected and name not in args.selected:
        continue
    directory = args.output / name
    directory.mkdir(exist_ok=True)
    path = directory / "source.k"
    path.write_text(prefix + source)
    ir = directory / "source.ll"
    flags = {
        "debug": ["-O0"],
        "optimized": ["-O2"],
        "sanitized": ["--sanitize"],
        "thread-sanitized": ["--thread-sanitize"],
    }[args.variant]
    run(
        [
            args.compiler,
            path,
            *flags,
            "--test-entry",
            "main",
            "--emit-llvm",
            ir,
            "-o",
            directory / "compiled",
        ],
        directory,
        "compile",
    )
    text = ir.read_text()
    coordinating = name in (
        "deferred-source-expression",
        "tensor-item-coordinating",
        "deep-adapter-chain",
        "adapter-chain",
        "take-zero-no-advance",
        "adapter-error-union",
        "filter-error",
        "adapter-prefetch-close",
        "effects-and-close",
        "owned-buffer-capture",
        "parallel-across-yield",
        "prefetch-close-cancels-timer",
        "recursive-items-coordinating",
    )
    factory = (
        "kalandra_generator_task_create_v1" if coordinating else "kalandra_generator_create_v1"
    )
    if name == "immediate-materialization":
        assert "call void @kalandra_generator_create_v1(" not in text, name
        assert "call void @kalandra_generator_task_create_v1(" not in text, name
        assert "kalandra_collect_append_v1" in text, name
    else:
        assert "call void @" + factory + "(" in text, ("incorrect producer factory", name)
    if not coordinating:
        assert "call void @kalandra_generator_task_create_v1(" not in text, name
    binary = directory / ("program.exe" if os.name == "nt" else "program")
    native = {
        "debug": ["-O0"],
        "optimized": ["-O2"],
        "sanitized": ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
        "thread-sanitized": ["-O1", "-g", "-fsanitize=thread"],
    }[args.variant]
    run(
        [
            args.clang,
            "-std=c++23",
            *native,
            *(["-fms-runtime-lib=static"] if os.name == "nt" else ["-pthread"]),
            "-Wno-override-module",
            root / "GeneratorSourceDriver.cpp",
            ir,
            args.runtime,
            "-o",
            binary,
        ],
        directory,
        "link",
    )
    for workers in (1, 4):
        result = run(
            [binary, expected, workers],
            directory,
            f"run-{workers}",
            timeout=10
            if name in ("prefetch-close-cancels-timer", "adapter-prefetch-close")
            else 180,
        )
        text = result.stdout.decode().replace("\r\n", "\n")
        assert text == output + "PASS\n" and not result.stderr, result
    print("PASS", name, flush=True)
