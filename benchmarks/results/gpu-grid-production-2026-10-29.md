# Production aligned-grid Metal matrices

Apple M2 Max, 29 October 2026. Follow-up to the [general 2D experiments](gpu-grid-dispatch-2026-10-29.md). Whole-model and startup timings use the actual compiler/runtime build; the broader isolated screen and earlier copied-runtime studies are identified separately.

Promoted the aligned variant: final resident transformer time falls 7.75% at batch16 and 7.08% at batch64 (width512). Broader isolated reductions reproduce at 12.82–27.13%. General bounded 2D dispatch is not promoted. Input-upload gains are more variable, all-weight gains are near the timing floor, and warm-cache process startup costs about 0.4–0.5 ms more. Cold driver-cache preparation remains unmeasured.

## Change and contract

The existing M2 Max register16 selection now uses a two-dimensional grid for complete 16×16 output tiles. Removing tile-coordinate division/modulo and redundant boundary predicates preserves every multiply/add, reduction root, barrier and fused epilogue. Ragged outputs retain the original 1D entry. Grouped/skinny choices and other devices keep their previous policy. There is no shape whitelist or function-constant cache. One optional pipeline per eligible plain/fused entry is prepared at module load; no compilation or cache growth occurs during a task. Older bundles omit the new entries and retain their prior behavior.

No language, arithmetic, cancellation, fault-precedence, scheduling or invocation-ownership contract changes. The additional pipelines have explicit preparation-allocation checkpoints and bounded module lifetime. The register study measured 100→94 allocated registers, no spills and a 29.167→35.417% theoretical occupancy ceiling. Achieved occupancy did not increase; this does not prove occupancy caused the speedup.

## Correctness and fault qualification

Four focused CTest groups pass: epilogue code generation, arithmetic policy, Metal pipeline and prepared programs (about 546 seconds total). Runtime checks cover optimized, debug, address/undefined-sanitized and thread-sanitized modes, old/mixed/legacy bundles, allocation-failure sweeps, held cancellation/deadline/device failures, exact outputs, guards and cleanup. This is targeted qualification, not a rerun of the entire compiler suite.

The actual emitted plain kernel and three fused bodies byte-match the qualified transformation. 117 complete-transformer cases match the frozen baseline bit-for-bit and an independent oracle across thirteen shapes, three policies and three upload modes. Another 78 isolated matrix checks cover broader shapes, three patterns and both versions: all GPU output bits/guards are compared; the independent CPU tree checks every cell up to 32,768 output cells and deterministic dispersed cells plus corners above that. Exceptional values establish compatibility with the original GPU kernel, not new independent IEEE conformance.

## Complete transformer timings

Positive values mean less elapsed time; parentheses show winning paired blocks out of seven. Each block uses adjacent ABBA/BAAB fresh processes, identical batch counts, two warmups and three measured rounds. Frozen/repeat target 50 ms with a 512-call cap. The additional identical-build AA and confirmation target 200 ms with a 2,048-call cap after substantial variation appeared on unchanged fallback paths. All samples, including the initial 24% fallback regression, remain retained. AA uses the same current executable and bundle under both labels.

Timed calls check output shape, resource cleanup and submission count, not a numerical checksum. Full numerical checks are separate. All use the existing closed proof; ordinary cancellable controls are in the preceding experiment. Preparation and compilation are excluded from this table.

| M × width / upload | Frozen | Repeat | Long-batch AA | Long-batch confirmation |
|---|---:|---:|---:|---:|
| 1×512 / resident | -24.34% (3/7) | -1.00% (2/7) | +0.16% (4/7) | -0.12% (3/7) |
| 1×512 / input | -0.65% (3/7) | -6.96% (3/7) | +0.31% (5/7) | +0.52% (5/7) |
| 1×512 / all weights | -1.14% (3/7) | +0.50% (4/7) | +0.13% (4/7) | -0.23% (3/7) |
| 16×512 / resident | +9.10% (6/7) | +8.60% (6/7) | +0.22% (5/7) | +7.75% (7/7) |
| 16×512 / input | +5.09% (5/7) | -3.82% (3/7) | -1.01% (3/7) | +6.95% (5/7) |
| 16×512 / all weights | -1.42% (3/7) | +4.80% (5/7) | +0.59% (4/7) | +1.23% (4/7) |
| 64×256 / resident | +6.56% (5/7) | +2.34% (5/7) | +0.04% (4/7) | +2.12% (6/7) |
| 64×256 / input | +2.15% (4/7) | +0.99% (4/7) | -0.52% (3/7) | -0.19% (3/7) |
| 64×256 / all weights | +3.31% (5/7) | +0.71% (4/7) | -1.48% (0/7) | +2.16% (6/7) |
| 64×512 / resident | +11.72% (5/7) | +5.27% (4/7) | -1.23% (1/7) | +7.08% (6/7) |
| 64×512 / input | -0.43% (3/7) | +7.36% (6/7) | +2.27% (7/7) | +5.07% (4/7) |
| 64×512 / all weights | -6.79% (3/7) | +1.81% (5/7) | -0.10% (3/7) | +1.84% (7/7) |

The batch1/width512 and batch64/width256 cells retain previous kernel selection. Upload timings include memory movement and must not be substituted for resident results. This study does not refresh CPU/vendor-library comparisons or establish general LLM parity.

## Broader isolated matrix timings

Seven paired blocks and an independent repeat, 30 ms target capped at 256 calls. Every output/guard is checked outside timing. These are isolated kernel measurements, not complete-program gains.

| M×K×N | Paired reduction | Repeat reduction |
|---|---:|---:|
| 16×576×528 | +21.16% (7/7) | +20.91% (7/7) |
| 16×4096×4096 | +27.16% (7/7) | +27.13% (7/7) |
| 32×2112×528 | +19.94% (7/7) | +20.05% (7/7) |
| 256×512×256 | +13.08% (7/7) | +12.82% (7/7) |
| 1024×1984×1024 | +18.46% (7/7) | +18.02% (7/7) |

## Startup cost

Fresh-process wall time includes loader, module/pipeline preparation, inputs, three invocations and teardown. Seven ABBA/BAAB blocks; OS/Metal caches were already warm. It does not isolate PSO compilation or qualify genuinely cold driver-cache behavior. Added preparation cost remains a tradeoff.

| Batch / width512 | Original process ms | Current process ms | Paired time reduction |
|---|---:|---:|---:|
| 16 | 38.70 | 39.19 | -1.20% |
| 64 | 45.72 | 46.06 | -1.62% |

## Load and limits

The user confirmed competing video/photo/GPU apps were closed before these production runs. Tests, builds, captures and acceptance ran sequentially. Five-second process samples exclude the benchmark tree; CPU load does not establish idle GPU utilization. WindowServer and system/UI activity persisted. No samples were discarded, and the short-batch regressions are retained.

| Study | External CPU median | Maximum |
|---|---:|---:|
| frozen | 142.9% | 194.7% |
| repeat | 133.5% | 451.5% |
| settled-aa | 126.3% | 242.6% |
| settled-confirm | 119.9% | 280.2% |
| broad-paired | 141.7% | 226.9% |
| broad-repeat | 154.6% | 254.0% |

## Reproduce

Prepare a fresh directory with `grid_production_prepare.py --build DIR`, then run `grid_production_verify.py --build DIR --output FILE`. Use `grid_production_measure.py --verification FILE --build DIR --study frozen|repeat --output FILE`, then `grid_settled_measure.py` with studies `aa|confirm`. `grid_broad_screen.py` supports separate prepare/measure phases; `grid_startup_measure.py` records warm-cache process costs. All require immutable artifacts and fresh output paths. Run `python3 native/benchmarks/fair/grid_production_report.py` after every study completes to audit raw observations, pairs, hashes and qualification. Large generated binaries/captures stay ignored.

- [Audited results and hashes](gpu-grid-production-2026-10-29.json)
- [Qualification and emitted-body matches](gpu-grid-production-qualification-2026-10-29.json)
- [Full transformer verification](gpu-grid-production-verification-2026-10-29.json)
- [CTest log](gpu-grid-production-tests-2026-10-29.log)
