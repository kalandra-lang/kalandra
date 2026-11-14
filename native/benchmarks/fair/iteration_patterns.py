"""Check and time equivalent direct/lazy traversal patterns using frozen artifacts.

This supplements iteration.py with generator factories, early stop, continue, while producers,
read-only arrays, captured callbacks, nested producers and local outlined regions.
Every timed invocation validates the exact result; no allocation or cancellation
checks are disabled. Run controls and independent replications on an idle host.
"""

import argparse
import json
from pathlib import Path

import protocol as p

COUNT = 4096
IMPORT = 'from "std/iter" import Iterator,iter,range as steps,map as mapped,take\n'


def programs():
    fixtures = {}
    ordinary = (
        "  total=0\n  for i in range(4096):\n    total=total+i*i%97\n  return total\n"
    )
    consume = (
        "  total=0\n  for value in values:\n    total=total+value\n  return total\n"
    )
    expected = sum(i * i % 97 for i in range(COUNT))
    fixtures["factory"] = (
        ordinary,
        "  total=0\n  for value in values():\n    total=total+value\n  return total\n",
        expected,
    )
    fixtures["take"] = (
        ordinary,
        "  source=for i in range(8192):\n    yield i*i%97\n  values=take(source,4096)\n"
        + consume,
        expected,
    )
    controls = "    if i==4096:\n      break\n    if i%7==0:\n      continue\n    total=total+i*i%97\n"
    fixtures["break-continue"] = (
        "  total=0\n  for i in range(8192):\n" + controls + "  return total\n",
        "  values=for index in range(8192):\n    yield index\n  total=0\n  for i in values:\n"
        + controls
        + "  return total\n",
        sum(i * i % 97 for i in range(COUNT) if i % 7),
    )
    fixtures["while"] = (
        "  i=0\n  total=0\n  while i<4096:\n    total=total+i*i%97\n    i=i+1\n  return total\n",
        "  values=while true:\n    i=0\n    while i<4096:\n      yield i*i%97\n      i=i+1\n    break\n"
        + consume,
        expected,
    )
    array = "  data=[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]\n"
    fixtures["array"] = (
        array
        + "  total=0\n  for batch in range(256):\n    for value in data:\n      total=total+(value+batch)%97\n  return total\n",
        array
        + "  values=for batch in range(256):\n    for value in data:\n      yield (value+batch)%97\n"
        + consume,
        sum((value + batch) % 97 for batch in range(256) for value in range(1, 17)),
    )
    fixtures["callback"] = (
        "  factor=3\n  total=0\n  for i in range(4096):\n    total=total+i*factor%97\n  return total\n",
        "  factor=3\n  transform=fn(i:i64)->i64:return i*factor%97\n  values=mapped(iter(steps(4096)),transform)\n"
        + consume,
        sum(i * 3 % 97 for i in range(COUNT)),
    )
    fixtures["nested"] = (
        "  total=0\n  for row in range(64):\n    for col in range(64):\n      total=total+(row*64+col)%97\n  return total\n",
        "  values=for row in range(64):\n    for col in range(64):\n      yield (row*64+col)%97\n"
        + consume,
        sum(i % 97 for i in range(COUNT)),
    )
    fixtures["region"] = (
        "  seed=prefix()\n" + ordinary.replace("return total", "return total+seed"),
        "  seed=prefix()\n  values=for i in range(4096):\n    yield i*i%97\n"
        + consume.replace("return total", "return total+seed"),
        expected + 3,
    )
    fixtures["factory-arguments"] = (
        ordinary,
        "  total=0\n  for value in valuesWithCount(4096):\n    total=total+value\n  return total\n",
        expected,
    )
    fixtures["direct-adapters"] = (
        fixtures["callback"][0],
        "  factor=3\n  transform=fn(i:i64)->i64:return i*factor%97\n"
        "  total=0\n  for value in mapped(iter(steps(4096)),transform):\n"
        "    total=total+value\n  return total\n",
        fixtures["callback"][2],
    )
    result = {}
    for name, (direct, lazy, answer) in fixtures.items():
        prefix = (
            IMPORT
            + "fn prefix()->i64:\n  a=1\n  return a+2\nfn values()->Iterator[i64,never]:\n  return for i in range(4096):\n    yield i*i%97\n"
        )
        if name == "factory-arguments":
            prefix += "fn valuesWithCount(count:i64)->Iterator[i64,never]:\n  return for i in range(count):\n    yield i*i%97\n"
        result[name] = (
            {
                "direct": prefix + "fn run()->i64:\n" + direct,
                "lazy": prefix + "fn run()->i64:\n" + lazy,
            },
            answer,
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--headers", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--baseline-mode", choices=("direct", "lazy"), default="direct")
    parser.add_argument("--candidate-mode", choices=("direct", "lazy"), default="lazy")
    parser.add_argument("--max-iterations", type=int, default=1000000)
    parser.add_argument("--pattern", action="append", dest="selected")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixtures = programs()
    unknown = set(args.selected or ()) - fixtures.keys()
    if unknown:
        parser.error("unknown patterns: " + ", ".join(sorted(unknown)))
    if args.selected:
        fixtures = {
            name: value for name, value in fixtures.items() if name in args.selected
        }
    if args.build_only:
        if not all((args.compiler, args.headers, args.build_dir)):
            parser.error("building requires compiler, headers and build directory")
        directory = args.build_dir.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        compiler, headers = args.compiler.resolve(), args.headers.resolve()
        runtime = (
            compiler.parent.parent / "lib/kalandra/native/libkalandra-native-runtime.a"
        )
        driver = Path(__file__).with_name("ResumableCallDriver.cpp").resolve()
        for name, (sources, answer) in fixtures.items():
            for mode, source in sources.items():
                folder = directory / f"{name}-{mode}"
                folder.mkdir()
                path, ir = folder / "source.k", folder / "source.ll"
                path.write_text(source + "fn main()->i64:\n  return run()\n")
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
                assert p.run([folder / "standalone"]) == f"{answer}\n"
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
                p.observations(p.run([folder / "driver", 1, 1, answer]), answer, 1, 1)
        files = [path for path in directory.rglob("*") if path.is_file()]
        files.extend(
            (compiler, runtime, driver, Path(__file__).resolve(), *headers.rglob("*.h"))
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
    if not args.baseline or not args.candidate or args.max_iterations <= 0:
        parser.error(
            "measuring requires two frozen builds and a positive iteration cap"
        )
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
        for name in fixtures:
            relative = f"{name}-{args.baseline_mode}/source.k"
            assert (directories["baseline"] / relative).read_bytes() == (
                directories["current"] / relative
            ).read_bytes()

    def invoke(cell, count, rounds):
        label, name = cell
        answer = fixtures[name][1]
        driver = directories[label] / f"{name}-{modes[label]}" / "driver"
        return p.observations(
            p.run([driver, count, rounds, answer]), answer, count, rounds
        )

    result = p.measure_paired(
        [[label, name] for label in directories for name in fixtures],
        invoke,
        lambda cell: fixtures[cell[1]][1],
        blocks=9,
        rounds=3,
        target_ms=1,
        adaptive=True,
        max_iterations=args.max_iterations,
    )
    verify()
    result.update(
        manifests=manifests,
        modes=modes,
        contract="Equivalent source-level traversals; exact results, O2, closed invocation/ownership/cancellation lifecycle included; no performance claim for escaping or effectful producers.",
    )
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
