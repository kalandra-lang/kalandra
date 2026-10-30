"""Audit MPSGraph arithmetic-policy comparison, retaining failed shapes."""
import json
import struct
import protocol as p
R=p.ROOT/'benchmarks/results'
data=json.loads((R/'mps-matrix-numerical-2026-10-30.json').read_text())
for field in ('source_sha256','artifact_sha256'):
 for name,digest in data[field].items():assert p.sha(p.ROOT/name)==digest,name
assert data['qualification_sha256']==p.sha(R/'gpu-matrix-exact-model-2026-10-30.json')
assert not data['production_change'] and not data['performance_acceptance']
assert len(data['records'])==58
failed=[r for r in data['records'] if r.get('failed')]
assert {(r['name'],r['policy']) for r in failed}=={('cancellation-4096',v) for v in ('default','full')}
total=0
for r in data['records']:
 if r.get('failed'):
  assert '1024' in r['error'] and 'must be <= 704' in r['error']
  continue
 assert not r['differences']['naive'] and not r['differences']['blocked']
 assert r['status']['default_reduced_precision']==0 and r['status']['inputs_unchanged']
 path=p.ROOT/r['output'];assert p.sha(path)==r['output_sha256']
 m,k,n=r['shape'];words=struct.unpack('<'+'I'*(m*n*3),path.read_bytes())
 assert words[:m*n]==words[m*n:2*m*n]==words[2*m*n:]
 total+=m*n*3
text=f'''# MPSGraph numerical-policy comparison — 30 October 2026

Correctness-only experiment on the same 29 inputs as the [exact-integer oracle](gpu-matrix-exact-model-2026-10-30.md), using float32 MPSGraph matrix multiplication. Each case runs with both the ordinary default execution descriptor and an explicit compilation descriptor setting `MPSGraphReducedPrecisionFastMathNone`. The installed SDK aliases its default to None; each process independently observes the default field as zero. API and shader validation remain enabled. Production has no MPSGraph dependency and is unchanged.

**28 cases succeed in each policy, three repetitions each: {total:,} output cells. Every successful output matches both own-code SIMD prototypes**, with arbitrary NaN payloads treated as equivalent. The randomized matrices match cell by cell too. Inputs remain unchanged. Default and explicit full precision have the same observed outputs. These are empirical results on this device/shapes, not an Apple guarantee of a fixed accumulation order or cross-device reproducibility.

| Witness | Exact real result | Current fixed tree | MPSGraph default/full | Own SIMD |
|---|---:|---:|---:|---:|
| 512-term large cancellation | 256 | 0 | 1 | 1 |
| 512-term product-rounding witness | 256 | 0 | 1 | 1 |
| Negative-zero products | signed-zero convention | -0 | +0 | +0 |
| Intermediate overflow then cancellation | 0 | NaN | +infinity | +infinity |

The cancellation examples retain large relative errors even though ordinary conservative floating-point error bounds pass. Neither full precision nor a specified operation order is a promise of exact real arithmetic. The observed MPSGraph behavior matches our independently modeled fused accumulation, not our existing separately rounded products and balanced tree.

## Retained reference failure

The 32×4096×32 cancellation case aborts under Metal API validation in both policies: the selected MPSGraph kernel requests 1024 threads per threadgroup while its pipeline limit is 704. Both errors are retained in the JSON; neither is a successful numerical comparison. Validation was not disabled to make the case pass. The first attempt stopped at this failure; the final runner records per-case failures and continues. No reference failure is used as a Kalandra performance or accuracy win.

## Design implication

Kalandra's current fixed-tree/separately-rounded contract is more restrictive than the behavior measured in MPSGraph's full-precision configuration. It was a reproducibility baseline, not a mathematical requirement for matrix multiplication. Ordinary finite float32 multiplication rounds because its exact product need not fit in float32; FMA can retain that intermediate product through the addition and round once. Requiring a separately rounded product forbids that optimization. This does not imply permission to drop input mantissa bits, use FP16/TF32, ignore exceptional values or relax integer faults.

Recommended next design discussion: make full-precision optimized tensor accumulation the ordinary policy, with an explicit reproducible/fixed-tree policy for callers who need it. Define numerical/exceptional-value guarantees before changing production; retain independent error, adversarial, shape and whole-model qualification. These correctness runs provide no new timing or claim that the entire MPSGraph performance gap has closed.

Apple sources: [reduced-precision policies](https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphreducedprecisionfastmath), [full precision](https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphreducedprecisionfastmath/none). The matrix multiplication API documentation does not specify Kalandra's fixed tree. Hardware-policy behavior is additionally established by this experiment rather than inferred from that omission alone.
'''
(R/'mps-matrix-numerical-2026-10-30.md').write_text(text)
print(f'Audited {total:,} successful MPS output cells; two reference validation failures retained.')
