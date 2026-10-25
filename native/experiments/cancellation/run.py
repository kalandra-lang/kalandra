#!/usr/bin/env python3
"""Build restricted cancellation probes from real, unchanged language source."""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "benchmarks/.build/cancellation"
LLVM = pathlib.Path("/opt/homebrew/opt/llvm/bin")
MODES = ("floor", "current", "entry", "cycles", "budget", "guard", "page", "async", "async-unwind", "hybrid", "unwind-poll")


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, text=True, **kwargs)


def extents(binary):
    lines = run([LLVM / "llvm-nm", "--defined-only", "--numeric-sort", binary],
                capture_output=True).stdout.splitlines()
    symbols = []
    for line in lines:
        fields = line.split()
        if len(fields) == 3 and fields[1] in ("T", "t"):
            symbols.append((int(fields[0], 16), fields[2]))
    result = {}
    for address, name in symbols:
        if ".body.clear" not in name and not name.endswith(".boundary"):
            continue
        end = next((value for value, _ in symbols if value > address), None)
        if end is None:
            raise RuntimeError("No following text symbol: " + name)
        result[name.removeprefix("_")] = end - address
    return result


def build(kind, sanitizer="none"):
    sanitize = [] if sanitizer == "none" else (["-fsanitize=address,undefined", "-g"] if sanitizer == "address" else ["-fsanitize=thread", "-g"])
    runtime = ROOT / ("build/native/native/libkalandra-task-core-optimized.a" if sanitizer == "none" else "build/native/native/libkalandra-native-runtime-" + ("sanitized" if sanitizer == "address" else "thread-sanitized") + ".a")
    OUT.mkdir(parents=True, exist_ok=True)
    here = pathlib.Path(__file__).parent
    flags = run([LLVM / "llvm-config", "--cxxflags", "--ldflags", "--libs",
                 "core", "irreader", "analysis", "--system-libs"],
                capture_output=True).stdout.split()
    run([LLVM / "clang++", here / "Prepare.cpp", "-o", OUT / "prepare",
         *flags, "-std=c++20", "-fexceptions"])
    run([LLVM / "clang++", "-std=c++20", "-O2", *sanitize, "-ffixed-x28", "-c",
         here / "Probe.cpp", "-o", OUT / "probe.o"])
    run([LLVM / "clang", "-c", here / "Enter.S", "-o", OUT / "enter.o"])
    source = OUT / f"{kind}.original.ll"
    source_path = ROOT / "native/benchmarks/fair/source.k" if kind in ("polynomial", "branches", "euclid") else here / "workloads.k"
    run([ROOT / "build/native/native/kalandra", source_path,
         "--test-entry", kind, "--emit-llvm", source, *( ["--sanitize"] if sanitizer == "address" else ["--thread-sanitize"] if sanitizer == "thread" else ["-O2"]), "-o", OUT / "standalone"])
    for mode in MODES:
        ir = OUT / f"{kind}.{mode}.ll"
        binary = OUT / f"{kind}.{mode}"
        manifest = OUT / f"{kind}.{mode}.extents"
        def transform(extra=()):
            run([OUT / "prepare", source, ir, mode, *extra])
            run([LLVM / "clang++", "-O2", *sanitize, ir, OUT / "probe.o", OUT / "enter.o",
                 runtime,
                 "-lpthread", "-o", binary])
        transform()
        if mode not in ("floor", "current"):
            spans = extents(binary)
            manifest.write_text("".join(f"{name} {size}\n" for name, size in spans.items()))
            transform([manifest])
            if extents(binary) != spans:
                raise RuntimeError("Linked PC metadata changed instruction extents")


def probe(kind, mode, depth, repetitions, cancel=False, owned=False):
    args = [OUT / f"{kind}.{mode}", mode, depth, repetitions, int(cancel), int(owned)]
    try:
        result = run(args, capture_output=True, timeout=30)
        return {"kind": kind, **json.loads(result.stdout)}
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        return {"kind": kind, "mode": mode, "depth": depth, "error": str(error),
                "stderr": getattr(error, "stderr", None)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", default="polynomial")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--sanitizer", choices=("none", "address", "thread"), default="none")
    parser.add_argument("--cancel", action="store_true")
    parser.add_argument("--depth", type=int, default=14)
    parser.add_argument("--repetitions", type=int, default=100)
    args = parser.parse_args()
    if args.sanitizer != "none":
        OUT = OUT / args.sanitizer
    if args.build:
        build(args.kind, args.sanitizer)
    for mode in MODES:
        if args.cancel and mode == "floor":
            continue
        print(json.dumps(probe(args.kind, mode, args.depth, args.repetitions,
                               args.cancel, args.cancel)), flush=True)
