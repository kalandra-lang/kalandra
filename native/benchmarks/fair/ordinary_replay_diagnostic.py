"""Instrument guarded range transfers; never use these binaries for timings."""
import argparse
import json
import re
from pathlib import Path
import protocol as p

q = argparse.ArgumentParser(description=__doc__)
q.add_argument('--build-dir', type=Path, required=True)
q.add_argument('--compiler', type=Path, required=True)
q.add_argument('--output', type=Path, required=True)
a = q.parse_args()
assert not a.output.exists()
build = a.build_dir.resolve()
compiler = a.compiler.resolve()
runtime = compiler.parent / 'libkalandra-task-core-optimized.a'
driver = Path(__file__).with_name('OrdinaryReplayDiagnostic.cpp')
files, results = [], []
pattern = re.compile(r'musttail call double @_mlir_ciface_kalandra_test_entry_ordinary_v1\.wide\(i64 ([-%\w.]+), i64 ([-%\w.]+),')
for width in (32, 128):
    original = build / f'array{width}' / 'source.ll'
    instrumented = build / f'replay-{width}.ll'
    exe = build / f'replay-{width}'
    lines, sites = [], 0
    for line in original.read_text().splitlines():
        match = pattern.search(line)
        if match:
            lines.append(f'  call void @ordinary_replay_observer(i64 {match[1]}, i64 {match[2]})')
            sites += 1
        lines.append(line)
    assert sites >= 2, (width, sites)
    lines.append('declare void @ordinary_replay_observer(i64,i64)')
    instrumented.write_text('\n'.join(lines) + '\n')
    p.run([p.CLANG, '-std=c++23', '-O2', '-ffp-contract=off', '-Wno-override-module', '-pthread',
           f'-DWIDTH={width}', driver, instrumented, runtime, '-o', exe])
    rows = [json.loads(row) for row in p.run([exe]).splitlines()]
    assert len(rows) == 14
    results.append(dict(width=width, instrumented_sites=sites, cases=rows))
    files.extend([original, instrumented, exe])
p.evidence(a.output, dict(diagnostic_only=True, results=results,
    contract='Observer checks exact original scalar arguments before every guaranteed musttail range replay. Independent source-level arithmetic oracle and expected zero/one transfer count. No timing claims.'),
    [Path(__file__), driver], [compiler, runtime, *files], compiler)
