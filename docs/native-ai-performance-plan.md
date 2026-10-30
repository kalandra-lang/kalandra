# Native AI performance and release qualification

Work stays on `native` and PR #260 into `develop`. Do not merge or publish a
release without a separate instruction. MPSGraph/Accelerate are benchmark-only
references; production uses native kernels and the pinned MIT Steel subset.

## Current state

- CPU source-call and numerical optimizations are implemented. The retained
  [equivalent-source study](../benchmarks/results/wide-input-final-2026-10-28.md)
  remains workload-specific; it is not universal C++/Rust parity.
- GPU parameter storage, prepared execution, bounded backing reuse, command
  scheduling and closed synchronous entry are implemented. Their contracts live
  in the design documents and executable regression tests.
- Optimized hardware float32 matmul uses the qualified Steel profile on M2 Max.
  `--matmul-policy fixed-tree` preserves the earlier arithmetic; software mode
  forces fixed-tree. CPU matmul currently retains its existing tree. Native host
  clients rebuild matching headers/static archives. Scalar arithmetic, checked
  integers, ownership, fault precedence and cancellation remain unchanged.
- Primitive qualification passed 361 cases / 57,524,416 cells in each of four
  native modes. Full-model changing-input/cache checks, independent numerical
  references, sustained integrity, relocated packages and executable docs passed.
- Whole-model performance remains provisional: the latest idle-machine AA control
  failed. Large diagnostics suggest roughly 2.5× improvement over the previous
  implementation and about 17% more time than MPSGraph; small results are mixed.
  Do not rerun until a favorable result appears or advertise exact percentages.

[Current report, complete retained outcomes and reproduction](../benchmarks/results/steel-production-2026-10-30.md).

## Release gates

1. Affected GPU regressions passed; finish hosted macOS/Linux/Windows qualification.
2. Verify signed/notarized package contents, the MIT notice and representative
   CPU/Metal execution from the downloaded artifact. Keep macOS minimum 26.
3. Review small-workload observations and disclose measurement uncertainty. The
   large model alone cannot establish absence of regressions.
4. Keep the PR description current and preserve `Closes #13`, `Closes #233` and
   `Closes #135`. Commit/push qualified changes to `native`; do not merge or tag.

## Subsequent optimization

Investigate GPU execution-time variability before precise performance claims;
profile-only evidence shows larger spikes in GPU time with comparatively steady
host encoding. Then measure remaining pass/command costs and kernel/dataflow
opportunities against MPSGraph. Architectural changes must state any numerical,
cancellation or resource-contract change and qualify it explicitly.

Correctness precedes performance. Freeze matching artifacts; run timing alone;
retain all outcomes, including failures. Separate ordinary and cancellable entry
contracts, source algorithms and library comparisons, cold preparation and warm
execution, resident inputs and owned uploads. Use complete independent numerical
oracles, IEEE/tail/empty cases, allocation failures and applicable sanitizers.

The completed experiment diary and rejected prototypes are available through the
[research archive](../benchmarks/research-archive.json); they are not release code.
