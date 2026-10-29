# Strict FFN tile experiments

Apple M2 Max, 29 October 2026. **No production change is accepted.** All ten controls pass their numerical/guard qualification, but none provides a broad improvement over the existing register16 kernel. These are isolated matrix screens; the previous whole-transformer acceptance remains the current language result.

The initial adjacent-column tile is 16.74–51.43% slower across six shapes. Preserving contiguous lane accesses does not rescue it. A four-output tile is dramatically slower. Explicitly limiting compilation to the 256 threads actually dispatched leaves the original shader nearly flat and does not rescue the larger tiles. Register spills were not measured, so neither a spill nor an occupancy diagnosis is proven by these timings.

Smaller 32-leaf chunks are 7.97–11.91% slower in the initial run and 7.73–11.84% slower in a seven-block repeat. Both encountered external CPU spikes (583% and 602% across cores), so these are retained as contended diagnostics, not clean acceptance or a final rejection. Larger 128-leaf chunks improve two small cells by 4.60% and 5.17%, while regressing other cells by up to 42.92%; these wins remain visible but do not justify replacing the production policy. The wider FFN expansion and contraction—the target of this experiment—do not improve.

## All paired outcomes

Positive percentages mean less elapsed time. Parentheses show positive blocks out of five, or seven in the repeat. Each cell uses adjacent ABBA/BAAB fresh processes, two warmups and three measured batches; adaptive batches target 30 ms with a 256-invocation cap. The faster side determines the shared count. No outlier removal is applied.

| Variant | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |
|---|---:|---:|---:|---:|---:|---:|
| Adjacent columns, 16×32 | -44.18% (0/5) | -16.74% (0/5) | -51.43% (0/5) | -20.48% (0/5) | -23.73% (0/5) | -22.87% (0/5) |
| Spaced columns, 16×32 | -54.41% (0/5) | -20.44% (0/5) | -61.78% (0/5) | -25.03% (0/5) | -2.91% (0/5) | -27.99% (0/5) |
| Four outputs, 32×32 | -600.82% (0/5) | -936.45% (0/5) | -688.61% (0/5) | -434.50% (0/5) | -514.91% (0/5) | -489.71% (0/5) |
| Original kernel, limit 256 | +0.48% (4/5) | -1.68% (2/5) | -0.06% (1/5) | -0.44% (2/5) | -0.18% (1/5) | -0.14% (2/5) |
| Adjacent columns, limit 256 | -46.87% (0/5) | -16.15% (0/5) | -51.18% (0/5) | -21.41% (0/5) | -23.97% (0/5) | -23.25% (0/5) |
| Spaced columns, limit 256 | -54.66% (0/5) | -19.65% (0/5) | -62.27% (0/5) | -23.33% (0/5) | -3.20% (1/5) | -27.57% (0/5) |
| Four outputs, limit 256 | -623.90% (0/5) | -986.73% (0/5) | -708.55% (0/5) | -450.43% (0/5) | -534.05% (0/5) | -504.46% (0/5) |
| Two rows, 32×16 | -37.15% (0/5) | -108.44% (0/5) | -42.51% (0/5) | -20.88% (0/5) | -24.78% (0/5) | -23.98% (0/5) |
| 32-leaf chunks | -11.91% (0/5) | -8.62% (0/5) | -11.87% (0/5) | -10.41% (0/5) | -7.97% (0/5) | -11.57% (0/5) |
| 128-leaf chunks | +4.60% (5/5) | -35.59% (0/5) | +5.17% (5/5) | -41.39% (0/5) | -8.23% (0/5) | -42.92% (0/5) |
| 32-leaf repeat (contended) | -10.59% (0/7) | -8.95% (0/7) | -11.74% (0/7) | -11.84% (0/7) | -7.73% (0/7) | -11.29% (0/7) |

## Qualification and scope

There are 720 passing case executions: ten controls × twelve shapes × three input patterns × baseline/candidate. Every warmup and measured batch checks every output bit against the original GPU kernel, input immutability, and output guard bytes outside timing. Non-dyadic mixed-scale and signed-zero inputs additionally use a separately rounded CPU adjacent-pair oracle. IEEE-pattern inputs use bit equality to the existing GPU behavior; that pattern includes NaNs, infinities and subnormals and is not a new independent IEEE conformance proof.

The 128-leaf experiment keeps the original entry when K is not divisible by 128. Qualification includes those inactive cases, odd output tails and non-power-of-two contraction lengths. New variants are appended to copied shaders, and copied drivers select matching dispatch geometry. Production source generation, fusion, runtime ownership, cancellation, admission and default policy are unchanged. No whole-program speedup or new cancellation qualification is claimed from an isolated kernel screen.

One command buffer executes repeated matrix calls against preallocated private buffers. Invocation lifecycle, allocation, input uploads and readback are excluded from timed batches. CPU reference generation and readbacks occur between batches; this is not a steady application workload. These screens cannot replace end-to-end closed/cancellable transformer acceptance.

## Pipeline controls and load

The unvalidated original pipeline reports a 1,024-thread maximum and 8 KiB static threadgroup storage. Explicit controls report a 256-thread maximum; two-output variants use 12 KiB, four-output variants 16 KiB. Every dispatch still uses 256 threads. Validation instrumentation changes reported resource values, so those values are retained separately rather than treated as production resource counts.

The thread-limit control follows [Apple’s documented pipeline property](https://developer.apple.com/documentation/metal/mtlcomputepipelinedescriptor/maxtotalthreadsperthreadgroup). Apple also discusses the interaction of register pressure, threadgroup storage and compilation limits in [Metal Compute on MacBook Pro](https://developer.apple.com/videos/play/tech-talks/10580/). Here, changing that limit is not a demonstrated optimization. It does not measure register spills or prove that occupancy caused the regressions.

All builds and validation completed before the corresponding sequential timing series. No profiling or other local test/build jobs overlap these timings. Ordinary background applications remain active. The initial screen has only one late load snapshot; later series retain periodic external-load samples. These are not dedicated idle-machine measurements.

## Next decision

Keep register16 and its 64-leaf chunks. The 32-leaf control still needs a quiet confirmation before a final verdict. Before another tile policy, obtain direct evidence about register usage, spills and active waves on representative FFN shapes. Alternatively screen batched Q/K/V dispatch as an independent source of overhead, with full numerical and task-lifetime qualification. A future shape-specific 128-leaf policy would need longer independent replication, inactive-shape controls and whole-block/upload acceptance; its isolated small-cell gains are insufficient here. SIMD-group matrix instructions, FMA or reassociation require a separate explicit numerical contract, not an implicit replacement of strict arithmetic.

## Evidence

- [screen](gpu-output-reuse-screen-2026-10-29.json)
  - [External load](gpu-output-reuse-screen-load-2026-10-29.json)
- [spaced](gpu-output-reuse-spaced-2026-10-29.json)
  - [External load](gpu-output-reuse-spaced-load-2026-10-29.json)
- [quad](gpu-output-reuse-quad-2026-10-29.json)
  - [External load](gpu-output-reuse-quad-load-2026-10-29.json)
- [limit-register](gpu-output-reuse-limit-register-2026-10-29.json)
  - [External load](gpu-output-reuse-limit-register-load-2026-10-29.json)
- [limit-adjacent](gpu-output-reuse-limit-adjacent-2026-10-29.json)
  - [External load](gpu-output-reuse-limit-adjacent-load-2026-10-29.json)
- [limit-spaced](gpu-output-reuse-limit-spaced-2026-10-29.json)
  - [External load](gpu-output-reuse-limit-spaced-load-2026-10-29.json)
- [limit-quad](gpu-output-reuse-limit-quad-2026-10-29.json)
  - [External load](gpu-output-reuse-limit-quad-load-2026-10-29.json)
- [rows](gpu-output-reuse-rows-2026-10-29.json)
  - [External load](gpu-output-reuse-rows-load-2026-10-29.json)
- [leaf32](gpu-output-reuse-leaf32-2026-10-29.json)
  - [External load](gpu-output-reuse-leaf32-load-2026-10-29.json)
- [leaf128](gpu-output-reuse-leaf128-2026-10-29.json)
  - [External load](gpu-output-reuse-leaf128-load-2026-10-29.json)
- [leaf32-repeat](gpu-output-reuse-leaf32-repeat-2026-10-29.json)
  - [External load](gpu-output-reuse-leaf32-repeat-load-2026-10-29.json)
- [All qualification](gpu-output-reuse-qualification-2026-10-29.json)
- [Pipeline properties](gpu-output-reuse-pipeline-resources-2026-10-29.json)
