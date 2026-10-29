# Strict GPU shape specialization

**Experimental evidence, not a production promotion.** Apple M2 Max, 29 October 2026. Specialization preserves the original arithmetic and improves isolated kernels; the full transformer, upload controls, load noise and missing production gates are reported below.

## Changes and correctness

Two controls clone the existing register16 kernel: one fixes K with Metal function constants; the other fixes M/K/N. Arithmetic order, operand order, shared layout, 8 KiB allocation, barriers and 256-thread geometry remain unchanged. Strict floating point and no FMA contraction are retained. Pipeline creation is outside timed batches.

144 validation executions cover twelve aligned/odd shapes, three input patterns and baseline/candidate for both controls. Every output bit, unchanged input and output guard is checked. Mixed-scale finite and signed-zero data additionally use the independent CPU adjacent-pair oracle; exceptional values test compatibility with the existing GPU implementation, not independent IEEE conformance. Timed batches repeat these checks outside the clock.

The copied runtime prepares at most six extra PSOs per applicable plain/fused register16 entry at module load. Its six exact shapes are an experimental policy, not a proposed production whitelist. Original reflection validation is retained. Dispatch performs only an immutable bounded lookup; all other shapes and kernel selections retain generic fallbacks. No task-time compilation or cache growth is introduced. 117 complete-transformer cases match baseline bits and pass the independent oracle, across thirteen shapes, all three execution policies and all three upload modes.

## Actual resources

Xcode 27.1 GUI replay of the 64×2048×512 contraction. Compiler fields are manually transcribed; hardware occupancy comes from the exported encoder counters. Replay duration is not acceptance timing.

| Kernel | Allocated/high registers | Temporary | Spilled bytes | Theoretical occupancy | Measured occupancy |
|---|---:|---:|---:|---:|---:|
| Original | 100/100 | 52 | 0 | 29.167% | 24.70% |
| k | 92/92 | 48 | 0 | 35.417% | 24.85% |
| shape | 88/88 | 44 | 0 | 35.417% | 23.08% |

Both still report 80 static threadgroup-load instructions. Static ALU counts are 230 for K-only and 189 for full shape. The higher theoretical ceiling does **not** establish higher achieved occupancy: full-shape measured occupancy is lower than the original. Specialization also removes generic index/bounds/root-state work; these experiments do not isolate register allocation as the causal mechanism.

## Isolated paired results

Positive percentages mean lower elapsed time. Values are medians of seven adjacent ABBA/BAAB block ratios; parentheses show winning blocks. Two warmups, three measured rounds, identical paired batch counts, 30 ms calibration target capped at 256 calls. No samples are removed. The AA control executes the identical original kernel under both labels.

| Study | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |
|---|---:|---:|---:|---:|---:|---:|
| k | +7.71% (4/7) | +2.49% (5/7) | +2.91% (6/7) | +4.70% (7/7) | +4.25% (6/7) | +2.91% (7/7) |
| shape | +18.32% (7/7) | +14.73% (6/7) | +18.45% (7/7) | +13.43% (6/7) | +17.19% (7/7) | +11.46% (6/7) |
| shape-control | +0.71% (4/7) | -0.08% (3/7) | +0.21% (4/7) | -4.13% (2/7) | -0.30% (3/7) | -0.32% (2/7) |
| shape-replication | +19.58% (7/7) | +15.78% (7/7) | +19.66% (7/7) | +12.10% (5/7) | +16.97% (6/7) | +11.81% (6/7) |

## Whole transformer

Frozen and repeat compare the original runtime/bundle with the copied specialized runtime/bundle. Control uses the same copied executable for both labels, changing only the bundle. Ordinary uses the ordinary cancellable policy; the other studies use the existing closed proof. Seven ABBA/BAAB blocks, two warmups and three measured rounds; 50 ms calibration target, capped at 512 invocations. Full numerical output is qualified separately. Timed calls validate output shape, resource cleanup and submission count, not a value-derived checksum.

| M × width / uploads | Frozen | Same-runtime control | Ordinary | Replication |
|---|---:|---:|---:|---:|
| 1×512 / resident | -3.04% (2/7) | +0.25% (4/7) | +6.30% (6/7) | -0.52% (3/7) |
| 1×512 / input | -2.21% (2/7) | -0.22% (3/7) | +1.50% (5/7) | -10.46% (2/7) |
| 1×512 / all weights | -0.44% (3/7) | +2.48% (5/7) | -3.53% (2/7) | -0.58% (1/7) |
| 16×512 / resident | +6.31% (5/7) | +7.33% (6/7) | +8.56% (5/7) | +6.33% (6/7) |
| 16×512 / input | +6.50% (6/7) | +6.13% (5/7) | +5.78% (5/7) | +5.01% (4/7) |
| 16×512 / all weights | -3.34% (1/7) | -0.26% (3/7) | +8.61% (5/7) | +3.10% (7/7) |
| 64×256 / resident | +3.00% (5/7) | +1.97% (5/7) | -4.57% (2/7) | +0.65% (4/7) |
| 64×256 / input | -2.89% (2/7) | +0.67% (4/7) | -0.84% (3/7) | +4.45% (5/7) |
| 64×256 / all weights | +1.60% (4/7) | -1.28% (1/7) | +1.20% (5/7) | +1.99% (5/7) |
| 64×512 / resident | +5.78% (6/7) | +7.58% (5/7) | +6.94% (6/7) | +14.77% (5/7) |
| 64×512 / input | +7.44% (4/7) | -6.12% (3/7) | +8.05% (6/7) | +5.43% (5/7) |
| 64×512 / all weights | +1.44% (5/7) | +4.19% (5/7) | +2.20% (5/7) | +1.76% (4/7) |

Shapes 1×512 and 64×256 do not match the specialization set and serve as fallbacks. Negative values and inconsistent upload outcomes are retained. These results do not establish a universal transformer gain or vendor-library parity. Setup, PSO creation, compilation and uploads preceding the measured invocation are excluded; all-weight upload within an invocation is included in its corresponding mode.

## Load and acceptance limits

All builds, validation, capture, profiling and timing are sequential. Background applications remain open. Five-second process samples exclude the benchmark process tree; 100% represents one CPU core. Media analysis, WindowServer and Chrome contribute substantial activity. GPU idleness is not established. Replication and controls quantify some noise but cannot remove contention.

| Study | External CPU median | Maximum |
|---|---:|---:|
| k | 347.1% | 416.5% |
| shape | 336.6% | 386.2% |
| shape-control | 413.2% | 435.1% |
| shape-replication | 341.4% | 351.0% |
| runtime-frozen | 323.9% | 510.9% |
| runtime-control | 408.3% | 553.0% |
| runtime-ordinary | 339.5% | 466.7% |
| runtime-repeat | 341.5% | 496.1% |

No production compiler/runtime source changes. Cancellation boundaries, fault precedence, task ownership and resource budgets are not altered in production. The prototype has not passed cancellation/fault-injection qualification or a full native suite, and its additional PSO setup/memory cost is not accepted. Numerical screening alone is insufficient for promotion.

## Next architectural decision

Full shape knowledge is useful, but hard-coding benchmark shapes is not a language optimization. Before designing a general bounded specialization cache, test a simpler general representation: two-dimensional threadgroup coordinates can remove group division/modulo by runtime tile-column count. A separately guarded full-tile variant can remove boundary predicates without knowing exact dimensions. Neither proposal is measured here. If dynamic specialization remains necessary, bound preparation and cache ownership and keep expensive compilation outside cancellable execution; qualify cold/setup cost, fallback, cancellation and allocation-failure precedence.

## Reproduction

Use `shape_specialization_screen.py --mode k|shape --phase prepare --build NEW_DIR --output NEW_DIR/qualification.json`, then capture/measure with the same frozen build. `fair/specialization_control.py` supplies the AA control. Profile captures in Xcode and stop replay before timing.

Use `shape_specialization_prototype.py --build NEW_DIR`, then `shape_specialization_verify.py` and `shape_specialization_measure.py`. Measurement currently expects the dated verification record; update that explicit path for a new study. Run `python3 native/benchmarks/fair/specialization_report.py` to audit retained local artifacts and regenerate this report. Large captures/binaries remain ignored; small exports, qualifications, all observations and hashes are committed.

- [Audited summary and hashes](gpu-specialization-2026-10-29.json)
- [Isolated qualification](gpu-specialization-qualification-2026-10-29.json)
- [Full-model qualification](gpu-shape-runtime-verification-2026-10-29.json)
- [Compiler fields](gpu-specialization-gui-2026-10-29.json)
- [Prior rejected resource-guided experiments](gpu-resource-guided-2026-10-29.md)
