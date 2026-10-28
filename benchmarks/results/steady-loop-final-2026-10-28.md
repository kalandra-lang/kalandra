# Stable integer recurrences and constant-round buffer folding

Final compiler source: `2840a4b804cb2f8071a83a744b795522211d0b74`; Windows-only test harness repair: `06475465`. Baseline compiler/runtime source: `1691341d233e48675890fab6fc310e9a78ad5942` (published evidence `1170774a`). Frozen artifacts ran on Apple M2 Max/macOS 27.2, Homebrew Clang 23.1.3 and Rust 1.93.0. These are warm ordinary standalone CPU calls with equivalent source algorithms, strict floating arithmetic and runtime arguments, not whole-process startup or GPU timings.

## Outcome

Both complete 36-case runs meet the 10% target. Across six independent process medians per language/case, **36/36 qualify and 31/36 are faster than both references**. The largest aggregate gap is **4.8%**, on the one-round polynomial. Individual runs and every zero/one-round case are retained below. This is qualification of these recorded workloads on this host, not a universal language ranking.

Paired one-round arrays at widths 32/64/128 take **94.0% / 97.3% / 98.6% less time** than the published compiler. Repeated GCD at 16/256 rounds takes **95.9% / 99.8% less time**, because its integer state reaches a fixed point and remaining pure iterations can be omitted. The new changing-state GCD control takes **12.9% / 6.3% less time** at 16/256 rounds, with all 256 iterations still doing arithmetic. Its one-round case returns to baseline (+0.1% paired improvement); the initial generalized candidate had regressed it by 19.2% and is retained as rejected. The worst final paired regression is **1.8%**, on one-round repeated GCD; no final corpus case exceeds 2% paired regression.

All eight established array/closure controls also meet the 10% cross-language target. Their task-scope diagnostics include fresh scope/pool/monitor construction and settlement; they are neither cross-language rankings nor steady-state scheduler throughput.

## Implementation and safety boundary

- Private clones guard nonnegative 16-bit data arguments; unrestricted inputs retain their original wide implementation, whose performance is not represented by the seed-17 timings. SSA recurrence proofs permit unsigned hardware remainder. A Euclidean `(a,b) -> (b,a%b)` pair uses 32-bit remainder only when both initial values fit; the recurrence cannot grow beyond those bounds. Other arithmetic stays wide. Positive counts extend through `INT64_MAX`, with unrestricted-input replay for values outside the proof domain.
- An expensive ordinary integer countdown can exit when all remaining data state is unchanged. The counter must affect only its decrement and termination test. Admission rejects memory effects, calls, allocation, floating state and explicit undefined/poison state. Managed cancellation/progress paths retain their existing behavior.
- Exact one-round integer proofs run before widening loop-carried ranges. Specializing an already-generalized body did not recover the original operand widths and was rejected.
- Speculative LLVM unrolling now accepts a proven maximum trip count, retaining early exits, as well as an exact count. The total expansion budget is 8,192 instructions; individual loops remain capped at 128 trips and 32 instructions. Accepted folded bodies still require no loops or allocations, at most 64 instructions, a threefold reduction and the existing native frame check. Rejected candidates keep their original implementation.
- Compact zero/one-round buffer workers use mandatory tail transfers. The 32-element one-round program becomes `416 * seed + 6672` followed by the original integer-to-float conversion. Allocation-failure policy and full-input/checked replay remain intact. Floating ordering, overflow faults, first-fault locations, ownership and cleanup contracts were not weakened.

The corpus now contains nine programs × four counts. `euclid_mixed` updates `x` with `(x*48271 + gcd(x*13+7,seed*7+11) + seed) % 2147483647`; an independent divisor oracle verifies 257 distinct states for seed 17 through round 256. It prevents the stable-GCD result from being presented as general sustained GCD throughput.

## Measurements

Times below are nanoseconds per call, using the median of six process medians from two complete runs. Reference difference is against the faster reference; negative means Kalandra takes less time. Positive paired reduction means improvement against the frozen published compiler. Each complete run uses three independent processes per cell, two warmups and seven measured adaptive batches targeting 10 ms. Nine adjacent ABBA/BAAB blocks use identical iterations per pair. Every warmup and timed return bit and wrapping checksum is checked. Additional seeds/counts use independent oracles; the auditor recomputes Fraction/modular-power/divisor results and statistics and checks artifact digests. Builds, tests and profiling stopped for timing; CPU affinity, frequency and desktop activity were not isolated.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | Reference difference | Paired reduction |
|---|---:|---:|---:|---:|---:|---:|
| affine | 0 | 0.906 | 1.209 | 1.218 | -25.02% | +0.80% |
| affine | 1 | 0.908 | 0.914 | 0.912 | -0.40% | +0.06% |
| affine | 16 | 4.357 | 8.031 | 7.988 | -45.46% | +0.13% |
| affine | 256 | 383.874 | 380.215 | 378.996 | +1.29% | +0.08% |
| polynomial | 0 | 0.944 | 1.215 | 1.214 | -22.27% | -0.09% |
| polynomial | 1 | 0.950 | 0.911 | 0.907 | +4.80% | -0.61% |
| polynomial | 16 | 4.862 | 7.957 | 8.019 | -38.89% | -1.56% |
| polynomial | 256 | 371.323 | 373.025 | 385.552 | -0.46% | -0.19% |
| euclid | 0 | 1.214 | 1.216 | 1.212 | +0.19% | -1.49% |
| euclid | 1 | 3.071 | 3.340 | 3.987 | -8.03% | -1.75% |
| euclid | 16 | 7.123 | 168.019 | 163.666 | -95.65% | +95.87% |
| euclid | 256 | 7.052 | 3346.318 | 3230.464 | -99.78% | +99.79% |
| euclid_mixed | 0 | 1.212 | 1.212 | 1.209 | +0.23% | +1.62% |
| euclid_mixed | 1 | 2.996 | 3.430 | 4.256 | -12.65% | +0.12% |
| euclid_mixed | 16 | 210.150 | 212.402 | 208.389 | +0.85% | +12.93% |
| euclid_mixed | 256 | 4066.691 | 4165.657 | 4158.464 | -2.21% | +6.27% |
| array8 | 0 | 6.247 | 22.786 | 22.389 | -72.10% | +0.05% |
| array8 | 1 | 10.897 | 52.270 | 50.797 | -78.55% | +0.11% |
| array8 | 16 | 124.416 | 472.680 | 443.467 | -71.94% | -0.15% |
| array8 | 256 | 1945.711 | 7788.514 | 7404.053 | -73.72% | -0.84% |
| array16 | 0 | 12.735 | 25.900 | 24.387 | -47.78% | -0.12% |
| array16 | 1 | 27.442 | 62.870 | 55.239 | -50.32% | +0.28% |
| array16 | 16 | 260.207 | 583.349 | 512.310 | -49.21% | -0.53% |
| array16 | 256 | 3941.501 | 9607.084 | 9078.736 | -56.59% | +0.17% |
| array32 | 0 | 1.829 | 28.855 | 25.762 | -92.90% | +13.60% |
| array32 | 1 | 2.125 | 73.226 | 72.895 | -97.09% | +94.02% |
| array32 | 16 | 481.512 | 734.633 | 723.806 | -33.48% | +0.17% |
| array32 | 256 | 7518.932 | 11977.878 | 11608.569 | -35.23% | -0.10% |
| array64 | 0 | 1.825 | 39.837 | 31.112 | -94.13% | +24.09% |
| array64 | 1 | 2.122 | 108.420 | 93.366 | -97.73% | +97.31% |
| array64 | 16 | 939.077 | 1147.790 | 1119.949 | -16.15% | -0.25% |
| array64 | 256 | 14553.834 | 18403.135 | 17793.034 | -18.20% | -0.33% |
| array128 | 0 | 1.819 | 62.167 | 53.670 | -96.61% | +14.08% |
| array128 | 1 | 2.125 | 185.752 | 167.512 | -98.73% | +98.64% |
| array128 | 16 | 1797.552 | 2036.957 | 1948.610 | -7.75% | +0.50% |
| array128 | 256 | 27925.349 | 34804.845 | 32711.024 | -14.63% | -0.28% |

[Generated IR and hardware disassembly](steady-loop-machine-qualification-2026-10-28.json).

[Baseline](steady-loop-baseline-2026-10-28.json), [final run](steady-loop-final-2026-10-28.json), [repeat](steady-loop-final-repeat-2026-10-28.json), [nine-block pairs](steady-loop-final-paired-2026-10-28.json), [independent audit](steady-loop-final-broad-audit-2026-10-28.json).

[Control baseline](steady-loop-controls-baseline-2026-10-28.json), [final controls](steady-loop-final-controls-2026-10-28.json), [control pairs](steady-loop-final-controls-paired-2026-10-28.json), [control audit](steady-loop-final-controls-audit-2026-10-28.json).

## Rejected candidates

A split floating zero/one CFG made some short paths faster but regressed affine zero/16-round and polynomial zero-round cases. Reducing late AArch64 pool alignment did not produce a convincing paired gain; inlining general buffer workers reserved their large frames before the zero-round decision. Those changes were removed. [Source snapshots and rejected patch](steady-loop-rejected-qualification-2026-10-28.json). [First prototype](entry-proof-current-2026-10-28.json), [paired data](entry-proof-paired-2026-10-28.json).

The first positive-count generalization improved multi-round work but displaced exact one-round width proofs: changing-state GCD/1 regressed 19.2%. The corrected compiler retains both proofs. [Intermediate run](steady-loop-current-2026-10-28.json), [repeat](steady-loop-repeat-2026-10-28.json), [pairs](steady-loop-paired-2026-10-28.json), [audit](steady-loop-broad-audit-2026-10-28.json). These intermediate results are not final qualification.

## Qualification

All **432 selected CPU regressions passed**, with zero failures or skips. The final integer matrix passed **12,591,344 input cases** across debug, optimized, ASan/UBSan and TSan builds, including both floating return precisions, cycles, negative/wide data, counter boundaries, first faults, ledger settlement, and direct convergent trillion-round/`INT64_MAX` calls. First-fault comparisons span checked, recoverable and direct companions; a separate wide-integer oracle checks nonnegative-count numerical results.

Actual Windows debug/optimized/ASan-UBSan execution passed **9,443,508 integer cases**, plus scalar/typed-buffer/reduction/failure matrices and installed/downloaded package qualification. The first Windows attempt exposed a harness-only link to the unavailable two-word return ABI; it was repaired to match the supported ABI selection already used by the Windows scalar-loop matrix. Checked, scalar-fault and direct checks remain active there; SysV pair checks remain active on macOS/Linux.

[Windows qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40684792576) and [Linux qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40684792544) passed, including their downloaded packages. All **50 selected Metal-suite tests passed**, with zero failures or skips. GPU execution cases required this Mac’s physical Metal device. [macOS qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40684792563) passed installed/relocated and downloaded mixed CPU/Metal packages, Developer ID signing, notarization, stapling and assessment. Branch qualification did not publish a tagged release. All three hosted runs qualify source `06475465`; compiler implementation is unchanged from `2840a4b8` apart from the Windows-only test flag. All 1,652 frozen compiler/source/artifact hashes were checked; the sole source delta is the documented Windows test-harness fix.

[Source/artifact manifest](steady-loop-qualification-2026-10-28.json), [numerical records](steady-loop-numerical-qualification-2026-10-28.json), [Windows records](steady-loop-windows-qualification-2026-10-28.json), [CPU log](steady-loop-cpu-qualification-2026-10-28.log), [CPU JUnit](steady-loop-cpu-qualification-2026-10-28.xml), [full build](steady-loop-build-qualification-2026-10-28.log), [Metal log](steady-loop-metal-qualification-2026-10-28.log), [Metal JUnit](steady-loop-metal-qualification-2026-10-28.xml).
