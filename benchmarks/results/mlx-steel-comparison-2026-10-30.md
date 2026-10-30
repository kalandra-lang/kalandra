# Pinned MLX Steel kernel comparison — 30 October 2026

Historical study retained as release context. Its original raw data and retired
reproduction scripts are preserved in Git and the [research archive](../research-archive.json).
This is not a fresh run of the current release candidate.

## Scope and provenance

Upstream MLX commit `1ba7f969ad05d86decfeeb83787cc9b79e440962`, MIT license retained alongside the evidence. The upstream `GEMMKernel<float,float,...,float>` templates and transitive headers are unmodified. Our adapter only supplies the Metal entry point, buffers, parameters and threadgroup arrays; it does not implement or rewrite the matrix algorithm. Three existing tile configurations are screened: 64×64×16 (four SIMD groups), 32×64×16 (two groups), 64×32×32 (four groups). No full MLX runtime, NAX path, split-K selector, transpose/batch selector or autotuner is included. These are selected Steel kernels, not a claim to measure peak MLX framework performance.

All candidates and our unchanged shader source are compiled with the pinned Metal toolchain, Metal 3.2, `-fno-fast-math -ffp-contract=off`, macOS minimum 26. Explicit matrix primitives still perform fused accumulation. Shared upstream headers require native bfloat syntax; an initial Metal 2.3 build failed before measurement. The correct newer standard uses `metal3.2`, not `macos-metal3.2`. No shader was edited to bypass those requirements. Our rebuilt current/SIMD outputs are checked against their earlier frozen Metal 2.3 numerical results.

## Correctness and behavior

**202 kernel/case checks, 26,819,304 output cells across three repetitions**, under Metal API and shader validation, with unchanged inputs and input/output suffix guards. This includes all 29 prior arithmetic witnesses, eight irregular shapes down to 1×1×1, and all five dense benchmark inputs. Each available implementation runs each case. Our aligned current kernel is used in timed cases; the existing guarded 8×8 production kernel supplies boundary-case comparison. Our aligned SIMD prototype is not dispatched on unsupported shape tails. All tested dimensions are positive; empty dimensions, transposes, batches and other element types are outside this screen.

Every bounded finite case passes a complete per-cell exact-integer oracle: float32 values are decoded at scale 2^-149 and exact products/sums at scale 2^-298, with the conservative gamma(K)×sum(abs(products)) bound compared in integer arithmetic. That bound is not a small-relative-error guarantee. Subnormal, overflow, infinity, NaN and signed-zero cases are retained as behavioral comparisons, not incorrectly admitted to the ordinary finite bound.

| Witness | Current fixed tree, first cell | Own SIMD | Steel configurations |
|---|---|---|---|
| negative-zero | `80000000` | `00000000` | steel64: `00000000`, steel32x64: `00000000`, steel64x32: `00000000` |
| cancellation-512 | `00000000` | `3f800000` | steel64: `3f800000`, steel32x64: `3f800000`, steel64x32: `3f800000` |
| fused-rounding-witness | `00000000` | `3f800000` | steel64: `3f800000`, steel32x64: `3f800000`, steel64x32: `3f800000` |
| intermediate-overflow-cancellation | `7fc00000` | `7f800000` | steel64: `7f800000`, steel32x64: `7f800000`, steel64x32: `7f800000` |
| subnormal-input-scaled | `00000000` | `00000000` | steel64: `00000000`, steel32x64: `00000000`, steel64x32: `00000000` |

Across the prior 29-case corpus, Steel has 0 differing output cells from the independently modeled own SIMD outputs (NaN payloads allowed). This observed equivalence is not a cross-device accumulation-order contract. The exact-256 cancellation witnesses and overflow differences from the current fixed tree remain visible; no universal accuracy claim follows.

## Matched uninstrumented timing

Same executable, private resident operands and preallocated output, same input bytes, API/graph setup and initial uploads excluded. Each timed command buffer contains identical dispatch counts; one compute encoder per dispatch for every implementation, followed by completed GPU acknowledgement. Readback, guards, repeatability and immutable-input checks occur outside the interval on every warm/measured round. No counter sampling or validation is enabled during timing. This is batched isolated-kernel latency, not Kalandra invocation or transformer throughput. Time columns are independent marginal medians; speedups are medians of paired ratios and need not equal the quotient of those marginal medians.

Screen: three adjacent ABBA/BAAB blocks per tile/shape. Select the highest median paired speedup per shape from that screen, freeze the selection, and use fresh five-block confirm/prototype/repeat studies. AA has three blocks of the identical rebuilt current kernel. Every pair uses identical iterations, two warmups, three measured rounds, a 100 ms adaptive target and 256-call cap. All outcomes and external CPU-load samples are retained. CPU samples cannot prove GPU idleness.

| Study | M × K × N | Reference µs | Steel/current µs | Reference / candidate | Block range |
|---|---|---:|---:|---:|---:|
| aa selected | 64 × 512 × 2048 | 88.18 | 88.38 | 0.998× | 0.998–1.012× |
| aa selected | 64 × 2048 × 512 | 94.45 | 94.42 | 1.001× | 0.999–1.009× |
| aa selected | 512 × 512 × 512 | 159.76 | 160.45 | 1.002× | 0.993–1.002× |
| aa selected | 512 × 512 × 2048 | 611.23 | 613.05 | 0.995× | 0.990–0.997× |
| aa selected | 512 × 2048 × 512 | 627.81 | 626.52 | 1.002× | 1.000–1.003× |
| screen steel32x64 | 64 × 512 × 2048 | 88.24 | 32.87 | 2.685× | 2.639–2.728× |
| screen steel64 | 64 × 512 × 2048 | 88.06 | 32.72 | 2.691× | 2.607–2.702× |
| screen steel64x32 | 64 × 512 × 2048 | 89.12 | 30.20 | 2.996× | 2.892–3.001× |
| screen steel32x64 | 64 × 2048 × 512 | 94.65 | 112.02 | 0.845× | 0.840–0.847× |
| screen steel64 | 64 × 2048 × 512 | 94.94 | 116.64 | 0.812× | 0.807–0.815× |
| screen steel64x32 | 64 × 2048 × 512 | 94.34 | 68.92 | 1.371× | 1.368–1.374× |
| screen steel32x64 | 512 × 512 × 512 | 161.59 | 45.93 | 3.518× | 3.474–3.706× |
| screen steel64 | 512 × 512 × 512 | 160.95 | 50.40 | 3.210× | 3.193–3.331× |
| screen steel64x32 | 512 × 512 × 512 | 161.16 | 40.03 | 4.026× | 3.975–4.070× |
| screen steel32x64 | 512 × 512 × 2048 | 725.71 | 126.26 | 5.755× | 5.241–6.136× |
| screen steel64 | 512 × 512 × 2048 | 732.04 | 127.03 | 5.770× | 5.540–5.911× |
| screen steel64x32 | 512 × 512 × 2048 | 747.91 | 130.56 | 5.693× | 5.432–5.769× |
| screen steel32x64 | 512 × 2048 × 512 | 712.23 | 162.39 | 4.386× | 3.903–4.572× |
| screen steel64 | 512 × 2048 × 512 | 724.23 | 178.86 | 4.049× | 3.797–4.217× |
| screen steel64x32 | 512 × 2048 × 512 | 727.87 | 143.70 | 5.065× | 4.719–5.408× |
| confirm selected | 64 × 512 × 2048 | 88.61 | 30.19 | 2.960× | 2.916–3.010× |
| confirm selected | 64 × 2048 × 512 | 94.41 | 69.15 | 1.365× | 1.357–1.370× |
| confirm selected | 512 × 512 × 512 | 160.00 | 40.41 | 3.970× | 3.933–4.027× |
| confirm selected | 512 × 512 × 2048 | 625.68 | 126.37 | 4.980× | 4.869–5.255× |
| confirm selected | 512 × 2048 × 512 | 631.25 | 143.79 | 4.389× | 4.348–4.532× |
| prototype selected | 64 × 512 × 2048 | 45.73 | 30.59 | 1.519× | 1.480–1.523× |
| prototype selected | 64 × 2048 × 512 | 136.59 | 69.00 | 1.979× | 1.964–2.011× |
| prototype selected | 512 × 512 × 512 | 61.33 | 39.96 | 1.534× | 1.523–1.540× |
| prototype selected | 512 × 512 × 2048 | 217.60 | 126.17 | 1.723× | 1.722–1.737× |
| prototype selected | 512 × 2048 × 512 | 235.06 | 143.44 | 1.641× | 1.638–1.649× |
| repeat selected | 64 × 512 × 2048 | 88.39 | 29.87 | 2.976× | 2.917–2.989× |
| repeat selected | 64 × 2048 × 512 | 94.32 | 68.63 | 1.375× | 1.365–1.377× |
| repeat selected | 512 × 512 × 512 | 159.32 | 39.78 | 4.004× | 3.934–4.018× |
| repeat selected | 512 × 512 × 2048 | 608.50 | 126.14 | 4.824× | 4.793–4.844× |
| repeat selected | 512 × 2048 × 512 | 623.09 | 143.10 | 4.353× | 4.339–4.370× |

The prototype study compares our faster blocked SIMD kernel with selected Steel; confirm/repeat compare the existing fixed-tree kernel with selected Steel. Those arithmetic contracts differ and are disclosed rather than treated as exact semantic substitutions.

## Control precision and frozen selection

| M × K × N | AA median time change | AA block range | Control | Selected tile |
|---|---:|---:|---|---|
| 64 × 512 × 2048 | -0.19% | -0.22% … +1.19% | pass | steel64x32 |
| 64 × 2048 × 512 | +0.07% | -0.06% … +0.84% | pass | steel64x32 |
| 512 × 512 × 512 | +0.17% | -0.70% … +0.23% | pass | steel64x32 |
| 512 × 512 × 2048 | -0.50% | -0.99% … -0.30% | pass | steel64 |
| 512 × 2048 × 512 | +0.24% | +0.02% … +0.29% | pass | steel64x32 |

No production source or arithmetic/cancellation contract changes. Adoption still requires a deliberate default numerical policy, integration with fused epilogues, resource/cancellation qualification, and full-model frozen/control/MPSGraph measurements. These sources provide a candidate starting point; they are not automatically correct for every Kalandra type, placement or numerical mode.

The user-supplied X article returned HTTP 403; its claims are not used as evidence for this benchmark. The tested upstream source and MIT license, compiler commands, input/artifact hashes, all variants and all selected/unselected results are retained.

## Interpretation

The large FFN expansion improves 4.980× in confirmation and 4.824× in replication versus the current tree; contraction improves 4.389× and 4.353×. Relative to the own blocked SIMD prototype, the same two kernels improve 1.723× and 1.641×. The selected Steel path wins all five measured shapes, while two unselected configurations lose on the small contraction. This supports using Steel as an integration starting point, not blindly selecting one tile everywhere.

Background CPU activity is present (median external load roughly 132–236% across cohorts, peaks up to 291%, where 100% is one core). All identical-kernel controls pass, and the large confirmation and replication wins agree, but these are not claims of a completely idle machine or universal performance. Screening baseline clock/load variation is retained; screening ratios are not substituted for the fresh confirmation results. No whole-model or MPSGraph-gap closure claim follows from these isolated kernel measurements.
