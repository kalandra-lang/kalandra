# General 2D GPU dispatch

Apple M2 Max, 29 October 2026. This section records the frozen experimental evidence; production qualification, if completed, is recorded separately below.

## Numerical and execution contract

Both variants use the original strict register16 arithmetic, reduction tree, shared tile, barriers and 256-thread geometry. A uint2 threadgroup coordinate replaces division/modulo by the runtime tile-column count. The lane uses thread_index_in_threadgroup, required for mixing a scalar lane index with vector grid coordinates. The aligned variant additionally removes output bounds predicates only for complete 16×16 tiles. No dimensions are function constants; no shape cache exists.

168 isolated validation executions cover fourteen shapes, three patterns and both versions. Full bits, input immutability and output guards pass; finite/signed-zero cases also match an independently rounded CPU tree. Exceptional values establish compatibility with the existing GPU kernel, not independent IEEE conformance. Isolated aligned-mode ragged cases use bounded 2D fallback. The whole-model prototype instead retains the original 1D fallback, because plain 2D did not establish a broad win.

117 complete-transformer cases pass exact baseline comparison and an independent oracle across thirteen shapes, three execution policies and three upload modes. The copied aligned-only runtime prepares one optional extra pipeline per eligible plain/fused entry at module load. It preserves other kernel choices, legacy fallback, command boundaries and task-time allocation behavior. Full cancellation/fault acceptance is separate from numerical verification.

## Resources

Xcode 27.1 full GUI replay of 64×2048×512. Compiler fields are manually transcribed, measured occupancy is from encoder exports. Replay times are not acceptance timings.

| Variant | Registers / high | Temporary | Spilled bytes | Theoretical occupancy | Measured occupancy | ALU instructions |
|---|---:|---:|---:|---:|---:|---:|
| Original | 100/100 | 52 | 0 | 29.167% | 24.70% | — |
| grid | 98/98 | 52 | 0 | 29.167% | 24.77% | 238 |
| aligned | 94/94 | 48 | 0 | 35.417% | 23.59% | 225 |

All retain 80 static threadgroup-load instructions. Register, instruction and occupancy changes are observations, not an isolated causal attribution.

## All isolated timings

Positive percentages mean less elapsed time; parentheses are winning blocks out of seven. Adjacent ABBA/BAAB fresh processes, two warmups, three measured rounds, 30 ms calibration target capped at 256 calls. Every output and guard is checked outside timing. AA executes the identical original kernel under both labels. No samples were discarded.

| Study | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |
|---|---:|---:|---:|---:|---:|---:|
| grid | -47.48% (3/7) | +2.13% (5/7) | -27.06% (2/7) | -16.45% (2/7) | +2.21% (5/7) | +0.78% (4/7) |
| aligned | +38.12% (6/7) | +5.75% (4/7) | +22.84% (6/7) | +9.31% (5/7) | +12.24% (4/7) | +27.53% (6/7) |
| aligned-control | -0.39% (3/7) | -0.09% (3/7) | +0.15% (4/7) | -0.86% (3/7) | -0.43% (1/7) | -0.04% (3/7) |
| grid-repeat | -0.93% (2/7) | +0.34% (5/7) | +0.16% (5/7) | -0.95% (2/7) | +5.36% (7/7) | -1.64% (0/7) |
| aligned-repeat | +20.32% (7/7) | +14.56% (7/7) | +23.22% (7/7) | +12.69% (7/7) | +17.28% (7/7) | +13.57% (7/7) |

The initial runs are highly variable and retained as contended screens. Replication stabilizes the baseline; AA medians remain within one percent. Plain 2D has little general benefit. Aligned tiles reproduce approximately 13–23% isolated reductions without an exact-shape whitelist.

## Whole transformer

Frozen/repeat compare the original executable/bundle with the copied aligned-only runtime/bundle. Control uses the same copied executable on both bundles. Ordinary retains the ordinary cancellable policy; other studies use the existing closed proof. Seven paired blocks, two warmups, three rounds; 50 ms target capped at 512 calls. Timed outputs check shape, resource cleanup and command count, not a numerical checksum; full numerical outputs are verified separately. Module load and pipeline compilation are excluded.

| M × width / uploads | Frozen | Same-runtime control | Ordinary | Repeat |
|---|---:|---:|---:|---:|
| 1×512 / resident | +1.77% (5/7) | +0.93% (4/7) | +0.09% (4/7) | +1.45% (4/7) |
| 1×512 / input | -2.88% (3/7) | -2.83% (3/7) | +2.40% (5/7) | -8.36% (3/7) |
| 1×512 / all weights | +0.83% (4/7) | +0.30% (4/7) | +0.38% (4/7) | +2.54% (5/7) |
| 16×512 / resident | +7.96% (7/7) | +7.47% (6/7) | +7.70% (7/7) | +7.89% (4/7) |
| 16×512 / input | +8.95% (7/7) | +6.80% (5/7) | +5.00% (5/7) | +3.26% (5/7) |
| 16×512 / all weights | +3.39% (6/7) | +4.01% (6/7) | +3.13% (7/7) | +0.80% (5/7) |
| 64×256 / resident | +2.97% (6/7) | +2.55% (5/7) | -0.53% (3/7) | +1.78% (5/7) |
| 64×256 / input | +1.10% (4/7) | +2.92% (6/7) | -6.07% (3/7) | -1.91% (3/7) |
| 64×256 / all weights | +0.29% (6/7) | +1.26% (5/7) | +2.42% (6/7) | +1.66% (4/7) |
| 64×512 / resident | +5.56% (7/7) | +7.12% (5/7) | +4.19% (6/7) | +1.94% (4/7) |
| 64×512 / input | +7.09% (6/7) | +7.35% (6/7) | +5.89% (6/7) | +4.32% (4/7) |
| 64×512 / all weights | +3.73% (5/7) | +5.01% (6/7) | +7.05% (7/7) | +3.77% (4/7) |

The 1×512 and 64×256 fallbacks are retained, including regressions. Upload results are not interchangeable with resident results. No new CPU/vendor-library comparison or general LLM parity is claimed.

## Background load

Builds, validation, profiling and timing were sequential. Process samples every five seconds exclude the benchmark tree; 100% is one CPU core. Media analysis, video services, WindowServer and other applications remain active. An idle GPU is not established. All adverse observations remain recorded.

| Study | External CPU median | Maximum |
|---|---:|---:|
| grid | 357.2% | 459.6% |
| aligned | 365.5% | 455.4% |
| aligned-control | 414.4% | 777.8% |
| grid-repeat | 373.4% | 451.3% |
| aligned-repeat | 312.8% | 408.4% |
| runtime-frozen | 281.4% | 393.4% |
| runtime-control | 176.2% | 268.8% |
| runtime-ordinary | 165.4% | 285.7% |
| runtime-repeat | 201.6% | 409.3% |

## Reproduction and evidence

`grid_dispatch_screen.py --mode grid|aligned --phase prepare|capture|measure --build DIR --output FILE` creates and checks frozen artifacts. `fair/grid_control.py` supplies AA. `grid_dispatch_prototype.py --mode aligned --build DIR` prepares the whole-model copy; `grid_dispatch_verify.py` verifies outputs, then `grid_dispatch_measure.py --verification FILE --build DIR --study frozen|control|ordinary|repeat --output FILE` measures it. `fair/grid_dispatch_report.py` audits all original sources/artifacts and regenerates this experimental report. Source versions changed by later promotion are checked against the exact recorded Git revision. Large captures remain ignored.

- [Audited summary and hashes](gpu-grid-dispatch-2026-10-29.json)
- [All isolated qualification](gpu-grid-dispatch-qualification-2026-10-29.json)
- [Whole-model verification](gpu-grid-runtime-verification-2026-10-29.json)
- [Compiler observations](gpu-grid-dispatch-gui-2026-10-29.json)
