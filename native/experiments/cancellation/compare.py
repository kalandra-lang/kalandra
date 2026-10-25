#!/usr/bin/env python3
"""Fresh-process ABBA comparison with ordinary C++/Rust source algorithms."""
import json
import platform
import datetime
import pathlib
import statistics
import sys
import run as probe
import qualify

sys.path.insert(0, str(probe.ROOT / "native/benchmarks/fair"))
import protocol as p


def main():
    output = pathlib.Path(sys.argv[1])
    assert not output.exists()
    source = probe.ROOT / "native/benchmarks/fair"
    build = probe.OUT
    p.run([p.CLANG, "-std=c++23", "-O2", "-ffp-contract=off", "-c",
           source / "Source.cpp", "-o", build / "cpp.o"])
    p.run([p.CLANG, "-std=c++23", "-O2", source / "SourceDriver.cpp", build / "cpp.o",
           "-o", build / "cpp"])
    p.run([p.RUST, "--edition=2021", "-C", "opt-level=2", "-C", "lto=off",
           source / "source.rs", "-o", build / "rust"])
    cells = [[version, lang, kind, depth] for version in ("baseline", "current")
             for lang in ("cpp", "rust") for kind in range(3) for depth in (10, 14)]

    def invoke(cell, count, rounds):
        version, lang, kind, depth = cell
        if version == "baseline":
            text = p.run([build / lang, kind, depth, count, rounds, 17])
            return p.observations(text, qualify.reference(qualify.KINDS[kind], depth, 17),
                                  count, rounds)
        observations = []
        for round in range(-2, rounds):
            p.COMMANDS.append(list(map(str, [build / f"{qualify.KINDS[kind]}.hybrid",
                "hybrid", depth, count, 0, 0])))
            row = qualify.invoke(qualify.KINDS[kind], "hybrid", depth, count)
            observations.append(dict(round=round, ns=row["ns"], checksum=row["checksum"]))
        return observations

    result = p.measure_paired(cells, invoke, lambda cell: qualify.reference(
        qualify.KINDS[cell[2]], cell[3], 17), blocks=3, rounds=1, target_ms=15)
    result["contract"] = "Same unchanged source recursion/leaf algorithms; ordinary optimized C++/Rust without cancellation; experimental cancellable Kalandra hybrid with setup/cleanup included. Restricted Darwin ARM64 prototype, not production TaskCore integration. Every timing checksum verified exactly."
    result["prototype_hashes"] = {p.key(path): p.sha(path) for path in
        [*pathlib.Path(__file__).parent.glob("*"), *[build / f"{kind}.hybrid" for kind in qualify.KINDS[:3]]]
        if path.is_file()}
    result.update(schema=1, system=platform.platform(),
        timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        compiler_sha256=p.sha(probe.ROOT / "build/native/native/kalandra"),
        reference_sha256={p.key(path):p.sha(path) for path in
            [source / "Source.cpp", source / "SourceDriver.cpp", source / "source.rs", build / "cpp", build / "rust"]},
        commands=p.COMMANDS, versions=dict(clang=p.run([p.CLANG,"--version"]),
                                          rust=p.run([p.RUST,"--version","--verbose"])))
    output.write_text(json.dumps(result, indent=2) + "\n")
    for lang in ("cpp", "rust"):
        for kind in range(3):
            for depth in (10, 14):
                ratios = [1 / r["speedup"] for r in result["pairs"]
                          if r["group"] == [lang, kind, depth]]
                print(lang, qualify.KINDS[kind], depth, statistics.median(ratios))


if __name__ == "__main__":
    main()
