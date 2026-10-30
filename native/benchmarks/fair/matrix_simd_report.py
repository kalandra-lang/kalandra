"""Reconstruct benchmark-only matrix architecture results; never production acceptance."""
import json
import statistics as st
from pathlib import Path

import protocol as p

R = p.ROOT / 'benchmarks/results'


def load(path):
    return json.loads(path.read_text())


def identity(data):
    for field in ('signature', 'artifacts', 'source_sha256', 'artifact_sha256'):
        for name, digest in data.get(field, {}).items():
            assert p.sha(p.ROOT / name) == digest, name


lines = [
    '# SIMD-group matrix architecture screen — 30 October 2026', '',
    'These own-code Metal prototypes are isolated preallocated kernels. Neither is '
    'selected by production, and neither establishes a whole-model MPSGraph speedup. '
    'The strict production baseline remains `8814a901`. See the '
    '[matched library investigation](gpu-kernel-library-gap-2026-10-30.md).', '',
    'The naive prototype uses one SIMD group per 8×8 output tile. Block32 stages '
    '32×32 panels in 8 KiB of threadgroup memory; four SIMD groups each maintain '
    'four 8×8 accumulators. Both use the Metal SIMD-group matrix primitive, without '
    'a vendor numerical library. This does not establish which physical arithmetic '
    'units execute it.', '',
    '## Numerical boundary', '',
    'Each prototype passes 20 cases: five shapes, ordinary finite or negative-zero '
    'inputs, and both implementations. Each case checks every cell in three '
    'rounds, unchanged inputs and output suffix guards. The strict baseline matches '
    'the balanced CPU arithmetic tree bit for bit. Candidate finite values satisfy '
    'a double-precision dot-product reference bound of gamma(K) × sum(abs(products)), '
    'where gamma(K) = K×2^-24 / (1−K×2^-24), plus 2^-100. This conservative bound '
    'does not establish a new language-wide numerical contract.', '',
    'Candidates change rounding and turn tested negative zeros into positive zeros. '
    'NaN, infinity, overflow, subnormal behavior, arbitrary dimensions and production '
    'cancellation/resource integration are not qualified here. They cannot silently '
    'replace the existing strict matmul.', '',
    '| Prototype | M × K × N | Baseline µs | Candidate µs | Speedup | Block range |',
    '|---|---|---:|---:|---:|---:|',
]
shapes = {(64,512,2048), (64,2048,512), (512,512,512),
          (512,512,2048), (512,2048,512)}
for stem in ('gpu-matrix-simd', 'gpu-matrix-simd-block32'):
    manifest = load(R / f'{stem}-manifest-2026-10-30.json')
    qpath = R / f'{stem}-qualification-2026-10-30.json'
    q = load(qpath)
    data = load(R / f'{stem}-paired-2026-10-30.json')
    for obj in (manifest, q, data):
        identity(obj)
    assert not q['production_change'] and not data['production_change']
    assert data['qualification_sha256'] == p.sha(qpath)
    assert {(tuple(x['shape']), x['pattern'], x['version']) for x in q['checks']} == {
        (shape, pattern, version) for shape in shapes
        for pattern in (0, 2) for version in ('baseline', 'current')}
    assert len(q['checks']) == 20
    for case in q['checks']:
        assert len(case['checks']) == 3
        for row in case['checks']:
            assert row['cells'] == case['shape'][0] * case['shape'][2]
            assert 0 <= row['max_error_bound_ratio'] <= 1
            if case['version'] == 'baseline':
                assert row['different_bits'] == 0
            elif case['pattern'] == 2:
                assert row['different_bits'] == row['cells']
    assert len(data['observations']) == 100 and len(data['pairs']) == 25
    for observation in data['observations']:
        p.observations('\n'.join(json.dumps(x) for x in observation['samples']),
                       observation['cell'][1]*observation['cell'][3],
                       observation['iterations'], 3)
    for pair in data['pairs']:
        sides = {'baseline': [], 'current': []}
        observations = [o for o in data['observations']
                        if o['process'] == pair['block']
                        and o['cell'][1:] == pair['group']]
        assert len(observations) == 4
        assert len({o['iterations'] for o in observations}) == 1
        for o in observations:
            sides[o['cell'][0]].append(st.median(
                s['ns']/o['iterations'] for s in o['samples'] if s['round'] >= 0))
        before, after = st.mean(sides['baseline']), st.mean(sides['current'])
        assert abs(pair['baseline_ns']-before) < .001
        assert abs(pair['current_ns']-after) < .001
        assert abs(pair['speedup']-before/after) < 1e-12
        assert abs(pair['time_reduction']-(1-after/before)) < 1e-12
    for shape in sorted(shapes):
        pairs = [x for x in data['pairs'] if tuple(x['group']) == shape]
        assert len(pairs) == 5 and {x['block'] for x in pairs} == set(range(5))
        ratios = [x['speedup'] for x in pairs]
        lines.append(f'| {stem} | {shape[0]} × {shape[1]} × {shape[2]} | '
                     f'{st.median(x["baseline_ns"] for x in pairs)/1000:.2f} | '
                     f'{st.median(x["current_ns"] for x in pairs)/1000:.2f} | '
                     f'{st.median(ratios):.3f}× | {min(ratios):.3f}–{max(ratios):.3f}× |')
    assert load(R / f'{stem}-paired-2026-10-30-load.json')['samples']

lines += ['', 'Five fresh adjacent ABBA/BAAB blocks per shape, two warmups and '
          'three measured rounds, identical iterations within each pair, 100 ms '
          'adaptive target and 256-call ceiling. Values are elapsed time per '
          'invocation, not instrumented encoder time. All samples and load '
          'records are retained; there is no separate identical-build control '
          'for these exploratory screens.', '',
          'Block32 substantially improves the large FFN kernels, but regresses '
          'the 64×2048×512 contraction. A universal shape policy is not justified. '
          'Do not multiply these speedups into model throughput: other kernels, '
          'epilogues, synchronization and runtime work remain.', '',
          '## Retained setup failures and next decision', '',
          'The first two attempts selected dispatch geometry using pipeline '
          'object identity. A separately created strict baseline pipeline '
          'therefore received 32 threads instead of 256 and failed output '
          'validation. The corrected geometry uses tile width and checks each '
          'pipeline’s capacity. Accuracy bounds were not loosened. Both failure '
          'logs remain beside these results.', '',
          'The load recorder context strings inherit an older study label; '
          'actual command lines and source/artifact hashes identify these '
          'experiments. No production source, numerical or cancellation contract '
          'changes in this investigation.', '',
          'Next: choose whether to expose an explicit optimized matrix operation '
          'while keeping strict matmul. Before promotion, define exceptional '
          'value/precision behavior, qualify boundaries and arbitrary shapes, '
          'preserve fused epilogues, and run independent full-model numerical '
          'checks plus frozen paired/control/MPSGraph measurements.']
(R / 'gpu-matrix-simd-architecture-2026-10-30.md').write_text('\n'.join(lines)+'\n')
print('Audited both SIMD prototypes: 40 numerical cases and 50 paired comparisons.')
