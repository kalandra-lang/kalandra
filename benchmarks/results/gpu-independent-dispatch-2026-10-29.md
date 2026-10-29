# Independent projection scheduling: whole-model gate

Apple M2 Max, 29 October 2026. **No production policy is promoted.** Isolated concurrent Q/K/V gains did not establish a whole-transformer improvement. The copied-runtime candidate remains an experiment; no language, cancellation, arithmetic, resource-limit or public ABI contract changes. All unfavorable outcomes are retained.

## Frozen and same-binary controls

The candidate groups three independent register16 float32 projections in one concurrent encoder, conditional on the existing closed synchronous proof. Shape guards remain in place; matching requires ready same-device operands, the same left operand and compatible shapes/kernel identity. Each output still uses the existing allocation/ownership machinery. Ordinary and bounded paths retain their original policy.

Seven ABBA/BAAB blocks per cell use 50 ms adaptive batches with a 512-invocation cap. Frozen compares the previously qualified binary with the prototype; control disables/enables grouping in the same prototype binary. Modes are 0: resident inputs, 1: fresh input upload, 2: fresh input/weight uploads. Shapes 1×512 and 64×256 do not group and are inactive controls. Positive reductions are faster. Paired medians are not ratios of the displayed timing medians.

| Study | Rows×width | Mode | Baseline µs | Candidate µs | Time reduction | Winning blocks |
|---|---|---:|---:|---:|---:|---:|
| frozen | 1×512 | 0 | 517.10 | 523.09 | -0.48% | 2/7 |
| frozen | 1×512 | 1 | 454.90 | 513.41 | +0.08% | 4/7 |
| frozen | 1×512 | 2 | 1793.74 | 1774.54 | +0.93% | 4/7 |
| frozen | 16×512 | 0 | 589.95 | 786.91 | -28.96% | 2/7 |
| frozen | 16×512 | 1 | 873.95 | 840.23 | +1.99% | 4/7 |
| frozen | 16×512 | 2 | 2081.96 | 2110.67 | -0.05% | 3/7 |
| frozen | 64×256 | 0 | 663.35 | 914.96 | -0.33% | 3/7 |
| frozen | 64×256 | 1 | 637.00 | 640.39 | -0.29% | 3/7 |
| frozen | 64×256 | 2 | 965.91 | 960.86 | -0.66% | 3/7 |
| frozen | 64×512 | 0 | 1056.21 | 1311.11 | -23.95% | 1/7 |
| frozen | 64×512 | 1 | 856.81 | 1164.16 | -23.60% | 2/7 |
| frozen | 64×512 | 2 | 2542.28 | 2525.86 | -2.56% | 2/7 |
| control | 1×512 | 0 | 411.50 | 513.20 | -7.44% | 2/7 |
| control | 1×512 | 1 | 415.59 | 413.13 | +0.59% | 6/7 |
| control | 1×512 | 2 | 1862.36 | 1807.15 | +2.39% | 5/7 |
| control | 16×512 | 0 | 891.07 | 998.94 | -3.98% | 2/7 |
| control | 16×512 | 1 | 661.71 | 803.50 | +2.58% | 4/7 |
| control | 16×512 | 2 | 2016.61 | 2057.19 | -0.14% | 3/7 |
| control | 64×256 | 0 | 669.28 | 657.69 | +1.73% | 6/7 |
| control | 64×256 | 1 | 675.63 | 684.95 | -1.38% | 3/7 |
| control | 64×256 | 2 | 1065.62 | 1071.28 | +0.17% | 4/7 |
| control | 64×512 | 0 | 1179.48 | 1363.01 | -4.40% | 2/7 |
| control | 64×512 | 1 | 1054.54 | 1262.27 | -19.70% | 1/7 |
| control | 64×512 | 2 | 2610.22 | 2518.70 | +5.44% | 5/7 |

Background activity and large block-to-block changes affect active and inactive cells. These are screening results, not uncontended acceptance. Their exact regression magnitudes should not be generalized. No local tests/builds/profiling overlapped the timed studies. Sampled external CPU load uses 100% per core:

- frozen: median 236.9%, maximum 494.8% (26 samples).
- control: median 213.7%, maximum 953.7% (24 samples).

## Correctness and limits

117 full-transformer cases pass: 13 shapes, three execution policies and three upload modes. Every final float32 bit matches the frozen baseline and passes the independent propagated-error oracle. Odd/inactive shapes and the active group trace are checked with Metal API/shader validation. This is not a new full native-suite qualification. Allocation-failure, cancellation and resource-bound promotion gates remain pending for the copied runtime. Timed calls validate shape, submission counts and resource cleanup, **not a numerical checksum**; full numerical checks are separate.

An initial 1024-wide whole-model attempt fails in the frozen baseline before candidate execution. A diagnostic of both binaries identifies `Error::Budget` (code 6) under the existing default 64 MiB budget. The original failed attempt and both probes are retained. This does not indicate a scheduling regression; isolated projections fit a different resource footprint.

## GPU command versus CPU encoding diagnostics

`Options.profile` records GPU command duration and CPU encoding duration. These measurements are separate from unprofiled screening and are not acceptance. GPU timestamp intervals are not occupancy or register measurements. The first diagnostic places most of the large candidate penalty inside the GPU command, rather than in CPU encoding. Separate parameter buffers do not eliminate the large-case penalty, so a shared parameter arena alone is not an adequate explanation.

### dispatch-profile

Cell fields: version, rows, width; final field is arena enabled for the arena control, or concurrent enabled for the read-only control. In the read-only control baseline/current mean writable/read-only declarations, both recompiled identically. Otherwise they mean grouping disabled/enabled. Values are medians of process medians, in µs.

| Cell | Wall | GPU command | CPU encoding |
|---|---:|---:|---:|
| baseline, 1, 512 | 396.82 | 155.94 | 39.16 |
| baseline, 16, 512 | 607.94 | 303.84 | 64.37 |
| baseline, 64, 256 | 642.93 | 315.38 | 67.01 |
| baseline, 64, 512 | 838.11 | 482.49 | 67.85 |
| current, 1, 512 | 394.43 | 156.99 | 38.41 |
| current, 16, 512 | 741.15 | 449.90 | 73.07 |
| current, 64, 256 | 633.24 | 316.07 | 63.55 |
| current, 64, 512 | 1282.42 | 917.25 | 99.25 |

### dispatch-arena-profile

Cell fields: version, rows, width; final field is arena enabled for the arena control, or concurrent enabled for the read-only control. In the read-only control baseline/current mean writable/read-only declarations, both recompiled identically. Otherwise they mean grouping disabled/enabled. Values are medians of process medians, in µs.

| Cell | Wall | GPU command | CPU encoding |
|---|---:|---:|---:|
| baseline, 16, 512, 0 | 641.77 | 306.00 | 85.52 |
| baseline, 16, 512, 1 | 604.83 | 305.06 | 61.68 |
| baseline, 64, 512, 0 | 874.53 | 483.96 | 89.44 |
| baseline, 64, 512, 1 | 852.13 | 485.79 | 73.58 |
| current, 16, 512, 0 | 582.71 | 275.42 | 93.15 |
| current, 16, 512, 1 | 587.25 | 302.38 | 71.67 |
| current, 64, 512, 0 | 1261.43 | 847.26 | 121.54 |
| current, 64, 512, 1 | 1533.55 | 1030.31 | 166.70 |

### readonly-profile

Cell fields: version, rows, width; final field is arena enabled for the arena control, or concurrent enabled for the read-only control. In the read-only control baseline/current mean writable/read-only declarations, both recompiled identically. Otherwise they mean grouping disabled/enabled. Values are medians of process medians, in µs.

| Cell | Wall | GPU command | CPU encoding |
|---|---:|---:|---:|
| baseline, 16, 512, 0 | 600.40 | 302.65 | 64.90 |
| baseline, 16, 512, 1 | 567.44 | 294.86 | 78.38 |
| baseline, 64, 512, 0 | 842.27 | 480.49 | 70.30 |
| baseline, 64, 512, 1 | 1541.67 | 995.68 | 143.71 |
| current, 16, 512, 0 | 601.54 | 300.82 | 63.36 |
| current, 16, 512, 1 | 892.70 | 538.60 | 114.56 |
| current, 64, 512, 0 | 845.19 | 480.14 | 71.37 |
| current, 64, 512, 1 | 1749.27 | 1215.23 | 147.47 |

## Read-only shader control

Pipeline reflection originally reports matrix inputs and shape parameters as read-write, although register16 only reads them. A copied shader adds `const` to those three arguments only; output remains writable. Both control and candidate bundles are recompiled with the same pinned compiler and strict options. 234 additional full-output cases (117 per bundle) match the original GPU bits and independent oracle. Reflection confirms read-only access for both inputs and parameters, with output still read-write. Serial timings remain comparatively close; concurrent results have large swings, including severe regressions. Read-only declarations do not reliably recover the lost performance. Reflection/access metadata alone does not prove a driver scheduling or hazard mechanism. The profile compares declaration variants in both serial and concurrent modes; it does not qualify a production optimization.

## Decision and next gate

Keep production scheduling and shaders unchanged. The isolated batched shader remains a distinct option: it places independent projections into one grid, rather than relying on concurrent-encoder scheduling. It still needs a copied whole-model integration and the complete ownership/fault/budget/cancellation gates. Do not relax strict reduction order or resource limits to recover a speedup. The exact cause of the concurrent whole-model penalty is not established by these controls.

## Reproduction and evidence

Build the copied runtime with `native/benchmarks/independent_dispatch_prototype.py --build PATH`, then run `independent_dispatch_verify.py` and sequential `independent_dispatch_measure.py --study frozen|control`. The profile driver builder is `independent_dispatch_profile_prepare.py`; the three diagnostic scripts use the dated frozen paths recorded in their source. `independent_readonly_probe.py prepare`, `verify`, then `measure` creates same-compiler shader controls. Prerequisite frozen program/bundle/task artifacts come from the closed-entry and attention studies. Never overlap timing with builds, tests or profiling. The initial profile collector was run from a temporary path; its committed `independent_dispatch_profile.py` copy has identical bytes. Generated binaries, shader bundles and captures stay ignored; tracked records retain hashes and commands.

- [gpu-independent-dispatch-verification-2026-10-29.json](gpu-independent-dispatch-verification-2026-10-29.json)
- [gpu-independent-readonly-verification-2026-10-29.json](gpu-independent-readonly-verification-2026-10-29.json)
- [gpu-independent-dispatch-budget-2026-10-29.json](gpu-independent-dispatch-budget-2026-10-29.json)
- [gpu-independent-readonly-access-2026-10-29.json](gpu-independent-readonly-access-2026-10-29.json)
- [gpu-independent-dispatch-frozen-2026-10-29.json](gpu-independent-dispatch-frozen-2026-10-29.json)
- [gpu-independent-dispatch-frozen-2026-10-29-load.json](gpu-independent-dispatch-frozen-2026-10-29-load.json)
- [gpu-independent-dispatch-control-2026-10-29.json](gpu-independent-dispatch-control-2026-10-29.json)
- [gpu-independent-dispatch-control-2026-10-29-load.json](gpu-independent-dispatch-control-2026-10-29-load.json)
- [gpu-independent-dispatch-profile-2026-10-29.json](gpu-independent-dispatch-profile-2026-10-29.json)
- [gpu-independent-dispatch-arena-profile-2026-10-29.json](gpu-independent-dispatch-arena-profile-2026-10-29.json)
- [gpu-independent-readonly-profile-2026-10-29.json](gpu-independent-readonly-profile-2026-10-29.json)
