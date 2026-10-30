"""Audit exact-oracle correctness evidence, including unfavorable cases."""
import json
from pathlib import Path
import struct
import protocol as p

R=p.ROOT/'benchmarks/results'
data=json.loads((R/'gpu-matrix-exact-model-2026-10-30.json').read_text())
for field in ('source_sha256','artifact_sha256'):
    for name,digest in data[field].items():
        assert p.sha(p.ROOT/name)==digest,name
assert not data['production_change'] and not data['performance_acceptance']
assert data['cases']==29 and len(data['records'])==87
names={r['name'] for r in data['records']}
assert len(names)==29
cells=0
for name in names:
    rows=[r for r in data['records'] if r['name']==name]
    assert len(rows)==3 and {r['variant'] for r in rows}=={'baseline','naive','blocked'}
    for r in rows:
        assert r['guards'] and r['inputs_unchanged'] and r['repeatable']
        m,k,n=r['shape'];cells+=m*n*3
        path=p.ROOT/r['output'];assert p.sha(path)==r['output_sha256']
        words=struct.unpack('<'+'I'*(m*n*3),path.read_bytes())
        assert words[:m*n]==words[m*n:2*m*n]==words[2*m*n:]
        assert sum(r['classes'].values())==m*n
        if r['mode']=='bounded':
            assert r['finite_accuracy_pass'] and not r['bound_failures'] and not r['nonfinite_cells']
        if r['mode']=='special':
            assert r['expected_class_pass']
        assert not (r['tree_model_mismatches'] if r['variant']=='baseline'
                    else r['sequential_fma_model_mismatches']),r

lines=['# Exact-oracle matrix correctness — 30 October 2026','',
       f'29 cases × three implementations × three executions: **{cells:,} output cells** '
       'checked, with Metal API/shader validation, unchanged inputs and input/output guards. '
       'These are correctness checks on frozen benchmark-only kernels, not performance '
       'acceptance or production promotion.','',
       '## What was independently checked','',
       'The oracle decodes each finite float32 into an exact integer times 2^-149, '
       'forms exact integer products and sums at scale 2^-298, and checks the finite '
       'error inequality using integers. No floating-point summation is used as '
       'ground truth. The decoder/rounder also has independent Decimal identity '
       'checks and explicit ties-to-even witnesses.','',
       'A second integer model implements nearest-even float32 operations with '
       'hardware flush-to-zero: (a) separately rounded products and the existing '
       'adjacent-pair tree; (b) sequential fused multiply-add from +0. '
       '**Every tested current-kernel output matches model (a), and every tested '
       'SIMD-prototype output matches model (b)**, allowing arbitrary NaN payloads. '
       'This is a measured model match on this device and corpus, not a promise '
       'that every future Metal implementation uses that accumulation order.','',
       'Coverage includes three independently randomized 64×576×64 problems, '
       'contraction lengths 512–4096, identity selection, ones, mantissa-sensitive '
       'inputs, cancellation, signed zeros, minimum normal/maximum finite values, '
       'subnormal inputs/results, infinities, quiet/signaling NaNs and overflow.','',
       '## Why passing an error bound is not universal accuracy','',
       '| Case | Exact real result, first cell | Current tree | SIMD prototype |','|---|---:|---:|---:|',
       '| Repeated `[1e20, 1, -1e20, 1]`, 512 terms | 256 | 0 | 1 |',
       '| Repeated `[-4096×4098, 4097×4097]`, 512 terms | 256 | 0 | 1 |',
       '| Small residual `[1, 2^-20, -1, 2^-20]`, 512 terms | 0.000244140625 | exact | exact |',
       '| Intermediate product overflow followed by cancellation | 0 | NaN | +infinity |','',
       'The first two examples pass the conservative sum-of-absolute-products '
       'bound despite approximately 100% relative error. Severe cancellation '
       'makes that bound large relative to the answer; this affects both '
       'implementations. “Strict” means a specified calculation order, not exact '
       'real arithmetic or guaranteed greater accuracy. The overflow example '
       'also demonstrates a categorical difference, beyond a few last bits.','',
       'On the three randomized cases, maximum per-cell relative errors for '
       'current/SIMD are:', '',
       '| Seed | Current | SIMD |','|---|---:|---:|']
for seed in range(3):
    rows={r['variant']:r for r in data['records'] if r['name']==f'random-exponents-{seed}'}
    lines.append(f'| {seed} | {rows["baseline"]["max_relative_error"]:.8g} | '
                 f'{rows["blocked"]["max_relative_error"]:.8g} |')
lines+=['','These maxima concern this test distribution, not general error guarantees. '
        'No evidence here supports automatically discarding input precision. The '
        'mantissa-sensitive witness is preserved, but it does not prove precision '
        'for every possible input.','',
        '## Exceptional values and scope','',
        'Both paths produce the expected result classes for the isolated infinity, '
        'NaN, zero-times-infinity and finite-overflow cases. Tested subnormal '
        'inputs/products flush to zero in both paths, consistent with the existing '
        'hardware policy; they do not meet a gradual-underflow real-number bound. '
        'Negative-zero sums stay negative in the current tree and become positive '
        'in the prototype. NaN payload equality and floating-point exception flags '
        'are not promised or tested.','',
        'The primitive is verified for the specified fused arithmetic on the '
        'tested aligned domain. It **does not implement current matmul semantics**, '
        'and is not universally more accurate. Production remains unchanged. '
        'A visible arithmetic-policy decision, shape-tail/fallback qualification, '
        'whole-model checks and cancellation/resource integration are still '
        'required before promotion.','',
        'The initial exact-bound-only run is also retained. Its original source '
        'is reconstructible from `matrix-exact-pre-model-2026-10-30.patch` applied '
        'to the final verifier; the second run adds the two independent operation '
        'models without changing the GPU kernels. An initial sandboxed invocation '
        'could not access Metal; rerunning the same binary with GPU access passed.']
(R/'gpu-matrix-exact-model-2026-10-30.md').write_text('\n'.join(lines)+'\n')
print(f'Exact-oracle audit: {cells:,} cells; tree and sequential FMA models match.')
