# Production Steel integration — 30 October 2026

Numerical qualification passed. Performance acceptance remains blocked by failed
identical-build controls, including a retry after the user confirmed the machine
was idle. Diagnostic timings below are not precise speedup acceptance claims.

The [release evidence bundle](release-evidence.json) retains the complete current
JSON studies, including all failed controls. JSON filenames below are document
keys inside that bundle, not separate tracked files. Raw local studies and retired
scripts are recoverable through the [archive record](../research-archive.json).

## Implementation and provenance

The compiler embeds fifteen transitive MIT-licensed MLX headers from commit
`1ba7f969ad05d86decfeeb83787cc9b79e440962`. Original source bodies and copyright
notices are retained; quoted includes are flattened and pragma-once directives
removed for embedding. The source inventory and original/body/embedded hashes
are in `native/third_party/mlx-steel/UPSTREAM.json`. The offline vendor audit also
compares the originals against that pinned checkout when supplied. The compiler
includes the MIT notice in generated source and bundle notices, and installs it
with the standalone Metal compiler. No NAX kernels, CUTLASS, CUDA DSL, MLX runtime,
MPSGraph or Accelerate code is imported into production.

The owned entry adapter retains Kalandra's twelve-byte shape ABI and existing
reflected buffers. It calls upstream loaders, GEMM loop, SIMD matrix arithmetic
and safe stores unchanged. Fused epilogues apply the existing validated post-map
to register fragments before storing, with bounds checks before capture reads.
Two measured layouts are available: 64×64×16 and 64×32×32, both with four SIMD
groups. This is selected upstream GEMM, not the entire MLX runtime or autotuner.

Only the qualified Apple M2 Max selects these entries. Eligible shapes have
M≥64, K≥64, N≥32 and signed-index-safe address products. Large FFN expansion
uses 64×64×16; the other admitted shapes use 64×32×32. Unsupported shapes,
unqualified devices and older bundles retain the original kernels. Loading
prepares a bounded eight plain or fused variants per applicable module, never
new pipelines at task time. Cold pipeline preparation is separate from warm
invocation timing and must be reported alongside it.

## Numerical and lifecycle contract

This deliberately changes the default **hardware float32 matmul** policy to
optimized full-precision accumulation: float32 operands/accumulators, fused
multiply-add and implementation-dependent contraction order. No float16/TF32
conversion or global fast-math flag is enabled. Scalar/post-map operations,
checked integers and `reduceSum` retain their prior rules. CPU matmul currently
retains the fixed tree under both policies; GPU float64 fallback is unchanged.

`--matmul-policy fixed-tree` retains separately rounded products and the existing
balanced contraction tree. Native Metal callers can also use
`Options::fixedTreeMatmul=true`. `--arithmetic software` always selects fixed-tree.
A fixed-tree matrix module cannot be relaxed by combining it with a newer
optimized epilogue module. Hardware fixed-tree still follows device underflow
and NaN behavior; use software mode for its gradual-underflow numerical contract.

Neither policy is exact real arithmetic or universally more accurate. Rounding,
signed zero, cancellation and intermediate-overflow/NaN behavior can differ.
The earlier independent exact-integer/APFloat study retains those differences;
they are not hidden behind a blanket relative-error tolerance.

No task, cancellation/checkpoint, failure-precedence, ownership or invocation
resource contract is changed. Existing fused boundaries stay fused, and the
adapter adds no argument buffers, command submissions or per-invocation cache.
Native C++ clients rebuild against the matching runtime headers/static archives
because `Options` gains a field; preserving the shader ABI does not promise
binary compatibility for previously compiled host client objects.

## Qualification

The focused primitive/lifecycle suite passed in optimized, debug, ASan/UBSan and
TSan modes: 361 cases and 57,524,416 output cells per mode (230,097,664 total).
It exercises both policies, all eight tile/alignment variants, large tails,
addressing bases, IEEE/cancellation/overflow patterns, fused epilogues, old bundles,
allocation failures, cancellation/deadlines, latched faults and mixed-policy
admission. APFloat supplies the FMA or fixed-tree/FTZ oracle.

Full-model qualification passed 1,944 changing-input/cache/policy output checks,
36 independent primary oracles, 128 additional outputs across all four native
build modes, and twelve MPSGraph reference oracles. Fixed-tree outputs match the
frozen previous implementation bit for bit. The propagated float32 error bound
is conservative and too loose to establish practical accuracy on its own; actual
observed maximum absolute error was approximately 6.14e-6. A separate sustained
4,059-invocation check verified fifteen complete outputs and all seven input
buffers, finding neither output drift nor input mutation.

An initial new lifecycle fixture wrongly expected an unlatched device error to
supersede cancellation already recorded before resume. It was corrected against
the unchanged TaskCore and fixed-tree baseline; production precedence was not
changed. The broad regression run passed 329 tests before being deliberately
stopped to prioritize measurements; it is not reported as a completed 344-test
run. The final rebuilt standalone package and relocated external-toolchain package
both passed, including the installed MIT notice, optimized policy manifest,
embedded Steel source and moved generated applications. CPU arithmetic-policy,
matmul epilogue codegen and vendor-license checks also passed (5/5). The website
build and type check passed. Executable documentation verification also passed:
20 pages, 44 native examples, 69 rendered blocks, 36 CPU programs and three GPU
programs, including the new fixed-tree command. Windows-only invocations and
source-install recipes retain their explicit host limitations.

Final affected GPU regressions also passed: public arithmetic policy, existing
fixed-tree optimization corpus, graph batching, prepared numeric programs and
encoding overlap (5/5). The moved cancellation fixture and benchmark-reference
suite passed; all fifteen retained benchmark CLI entry points passed smoke checks,
and the 41-row capability registry agrees with its fixtures/evidence/docs.
Website formatting/types/tests/build and all 28 release-workflow unit tests passed.
The release evidence bundle records these logs and executable-documentation checks.
Hosted platform/signing qualification is a separate exact-commit gate.

## Matched full-model acceptance

Three fresh-process identical-build control studies failed the predeclared
limits (median elapsed-time difference ≤3%, worst ≤6%). The third was explicitly
requested after the user confirmed the machine was idle; every workload still
failed, with worst deviations of 18.3–22.9%. A same-runtime/same-program control
experiment also failed. All raw runs, warmups and load samples are retained in
`steel-production-v1*-aa*.json` and `steel-persistent-v1*-aa*.json`. No precision
comparison was promoted through the failed gate, and no build/test/profiling job
overlapped these timings. CPU load samples alone cannot establish GPU idleness.

A separate **unqualified diagnostic** compared three alternating-order processes
of the complete 512×512 resident transformer, 128 invocations per round, two warm
rounds and five measured rounds. Process medians were:

| Implementation | Process medians (µs/invocation) |
| --- | --- |
| Frozen previous production | 2864.04 / 2840.42 / 2854.53 |
| Current Steel integration | 1092.31 / 1138.29 / 1110.59 |
| MPSGraph reference | 954.94 / 960.74 / 964.58 |

This suggests a substantial whole-model improvement and a remaining MPSGraph
gap, but the failed control prevents accepting an exact improvement percentage.
It does not establish small-workload or upload-path acceptance. Raw diagnostic
rows and artifact hashes are in `steel-whole-model-spot-2026-10-30.json`.

The subsequent portable wider diagnostic retains all three process medians for
all workloads, including unfavorable observations:

| Workload | Previous production | Current fixed-tree | Steel | MPSGraph |
| --- | --- | --- | --- | --- |
| 1×512 resident | 384.65 / 377.36 / 373.82 | 385.98 / 374.23 / 414.78 | 382.84 / 415.92 / 411.71 | 302.75 / 332.05 / 328.79 |
| 64×512 resident | 715.89 / 740.75 / 744.83 | 710.75 / 734.23 / 736.65 | 839.84 / 571.09 / 571.06 | 524.40 / 535.66 / 555.23 |
| 512×512 resident | 2882.07 / 2874.85 / 2891.09 | 2978.33 / 2871.17 / 2862.50 | 1147.88 / 1146.74 / 1128.03 | 978.93 / 984.92 / 962.56 |
| 512×512 owned uploads | 3447.65 / 3441.29 / 3431.08 | 4128.44 / 3554.22 / 3417.86 | 1730.59 / 1752.54 / 1685.98 | 2555.05 / 2544.56 / 2525.39 |

Units are µs/completed invocation. The one-row case appears slower despite not
selecting Steel, and the first 64-row Steel process is much slower than its later
two processes. These discrepancies reinforce the failed-control limitation;
they must not be erased or presented as proven regressions or improvements.
The large resident case remains directionally promising in both diagnostics.
`steel-whole-model-diagnostic-2026-10-30.json` records every raw round, artifact
identity and qualification hash. Neither diagnostic ran alongside builds/tests.
Profile-only investigations found spikes mainly in GPU execution time while CPU
encoding stayed comparatively steady; the source of that variation is unresolved.

The workload is the compiler's real transformer block: RMS normalization,
Q/K/V projections, attention/softmax, residual projection and ReLU FFN. Numerical
qualification reads complete outputs and uses an independent propagated
float32 error bound. Warm timing excludes graph/pipeline preparation and initial
resident uploads, includes completed invocation acknowledgement, and checks
shape/lifecycle/resource invariants. Numerical readbacks occur outside acceptance
timing; the timed checksum is a shape/work checksum, not a numerical checksum.

The all-upload guard includes owned input snapshots on both sides. Kalandra
reuses bounded backing; the MPSGraph reference allocates its owned shared uploads
per call. That difference is disclosed rather than presenting the two allocation
implementations as identical. MPSGraph remains a benchmark-only reference.
Its default graph compilation optimization level remains enabled.
`MPSGraphOptionsNone` omits implicit CPU-result synchronization; both sides
explicitly wait for completed GPU acknowledgement, and readback is excluded.

## Remaining gap and next decision

The isolated-kernel result is independently qualified in the earlier comparison;
whole-model diagnostic results must retain the failed-control limitation above.
Do not claim the integration closes the MPSGraph gap or publish exact whole-model
speedups yet. Resolve the GPU execution-time variability, then repeat the
prespecified controls and comparisons, including small shapes and owned uploads.

## Reproduction

Run from the repository root, using the pinned LLVM and Apple Metal toolchains.
The frozen old executable/bundle are comparison inputs; substitute their paths
when reproducing on a new checkout. Choose fresh output paths for each run.

```sh
python3 native/third_party/mlx-steel/vendor.py --upstream /path/to/pinned/mlx
python3 native/benchmarks/steel_integration.py --phase prepare --build benchmarks/.build/steel-new --output /private/tmp/steel-prepare.json --baseline /path/to/old/gpu --baseline-bundle /path/to/old/bundle
python3 native/benchmarks/steel_integration.py --phase verify --build benchmarks/.build/steel-new --output benchmarks/.build/steel-new/qualification.json --baseline /path/to/old/gpu --baseline-bundle /path/to/old/bundle
python3 native/benchmarks/fair/steel_integration_series.py --build benchmarks/.build/steel-new --prefix benchmarks/results/steel-new --baseline /path/to/old/gpu --baseline-bundle /path/to/old/bundle
```

The verification output must be the build directory's `qualification.json`, as
shown above. The series stops when its AA control fails. Do not override that gate to report acceptance.
For an explicitly unqualified diagnostic, run `steel_model_diagnostic.py` with
`--build`, `--baseline`, `--baseline-bundle` and a fresh `--output` instead.
The complete numerical qualification evidence is retained in
`steel-production-v1-2026-10-30-qualification.json`.
