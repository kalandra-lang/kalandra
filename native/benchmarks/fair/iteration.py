"""Frozen iteration comparisons; validate every warmup and measured result.

Build against an installed compiler prefix, as with resumable_calls.py. Compare
the same mode across frozen prefixes for regressions, or select direct/source/
adapters/materialize modes to quantify the cost of each abstraction. Different
modes deliberately have different storage and suspension behavior; their ratios
are not claims that those behaviors are interchangeable.
"""

import argparse
import json
from pathlib import Path

import protocol as p

MODES = ("direct", "source", "adapters", "materialize")
COUNTS = (64, 4096)


def source(mode, count):
    prefix = 'from "std/iter" import iter,range as steps,map as mapItems,filter\n'
    if mode == "direct":
        body = f"""  total=0
  for i in range({count}):
    if i%2==0:
      total=total+i*i%97
  return total
"""
    elif mode == "adapters":
        body = f"""  cursor=mapItems(filter(iter(steps({count})),fn(i:i64)->bool:return i%2==0),fn(i:i64)->i64:return i*i%97)
  total=0
  for value in cursor:
    total=total+value
  return total
"""
    else:
        creation = f"for i in range({count}):\n    if i%2==0:\n      yield i*i%97\n"
        if mode == "materialize":
            creation = "array(" + creation + "  )\n"
        body = (
            "  values="
            + creation
            + """  total=0
  for value in values:
    total=total+value
  return total
"""
        )
    return prefix + "fn run()->i64:\n" + body + "fn main()->i64:\n  return run()\n"


def expected(count):
    return sum(i * i % 97 for i in range(count) if i % 2 == 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--headers", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--baseline-mode", choices=MODES, default="direct")
    parser.add_argument("--candidate-mode", choices=MODES, default="direct")
    parser.add_argument("--target-ms", type=float, default=1)
    parser.add_argument(
        "--max-iterations",
        type=int,
        default=1000000,
        help="Bound adaptive batches for very different producer costs",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.max_iterations <= 0:
        parser.error("--max-iterations must be positive")
    if args.build_only:
        if not all((args.compiler, args.headers, args.build_dir)):
            parser.error("building requires --compiler, --headers and --build-dir")
        directory = args.build_dir.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        compiler, headers = args.compiler.resolve(), args.headers.resolve()
        runtime = (
            compiler.parent.parent / "lib/kalandra/native/libkalandra-native-runtime.a"
        )
        driver = Path(__file__).with_name("ResumableCallDriver.cpp")
        for mode in MODES:
            for count in COUNTS:
                folder = directory / f"{mode}-{count}"
                folder.mkdir()
                path, ir = folder / "source.k", folder / "source.ll"
                path.write_text(source(mode, count))
                p.run(
                    [
                        compiler,
                        path,
                        "-O2",
                        "--test-entry",
                        "run",
                        "--emit-llvm",
                        ir,
                        "-o",
                        folder / "standalone",
                    ]
                )
                assert p.run([folder / "standalone"]) == str(expected(count)) + "\n"
                p.run(
                    [
                        p.CLANG,
                        "-std=c++23",
                        "-O2",
                        "-Wno-override-module",
                        "-pthread",
                        "-iquote",
                        headers,
                        driver,
                        ir,
                        runtime,
                        "-o",
                        folder / "driver",
                    ]
                )
                p.observations(
                    p.run([folder / "driver", 1, 1, expected(count)]),
                    expected(count),
                    1,
                    1,
                )
        files = [path for path in directory.rglob("*") if path.is_file()]
        files.extend(
            (
                compiler,
                runtime,
                driver.resolve(),
                Path(__file__).resolve(),
                *headers.rglob("*.h"),
            )
        )
        (directory / "manifest.json").write_text(
            json.dumps(
                dict(
                    hashes={str(path): p.sha(path) for path in files},
                    commands=p.COMMANDS,
                ),
                indent=2,
            )
            + "\n"
        )
        return
    if not args.baseline or not args.candidate:
        parser.error("measuring requires --baseline and --candidate")
    assert not args.output.exists()
    directories = dict(
        baseline=args.baseline.resolve(), current=args.candidate.resolve()
    )
    modes = dict(baseline=args.baseline_mode, current=args.candidate_mode)
    manifests = {
        label: json.loads((path / "manifest.json").read_text())
        for label, path in directories.items()
    }

    def verify():
        for manifest in manifests.values():
            assert all(
                p.sha(path) == digest for path, digest in manifest["hashes"].items()
            )

    verify()
    if args.baseline_mode == args.candidate_mode:
        for count in COUNTS:
            relative = f"{args.baseline_mode}-{count}/source.k"
            assert (directories["baseline"] / relative).read_bytes() == (
                directories["current"] / relative
            ).read_bytes()
    cells = [[label, count] for label in directories for count in COUNTS]

    def invoke(cell, count, rounds):
        label, size = cell
        driver = directories[label] / f"{modes[label]}-{size}" / "driver"
        return p.observations(
            p.run([driver, count, rounds, expected(size)]),
            expected(size),
            count,
            rounds,
        )

    result = p.measure_paired(
        cells,
        invoke,
        lambda cell: expected(cell[1]),
        blocks=9,
        rounds=3,
        target_ms=args.target_ms,
        adaptive=True,
        max_iterations=args.max_iterations,
    )
    verify()
    result.update(
        manifests=manifests,
        modes=modes,
        contract=(
            "Filter even indices, map i*i%97, then sum; exact per-invocation checks. "
            "Kalandra closed runner, ownership and cancellation lifecycle included. "
            "Source and adapters are lazy; materialize stores a growing array. "
            "Run only on an idle host, sequentially with controls and replications."
        ),
    )
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
