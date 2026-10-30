# GPU kernel and numerical-library gap — 30 October 2026

Production source is `8814a901`. No new production optimization is accepted in this investigation. The same authored float32 transformer block is compared through warm synchronous tensor-result APIs, with resident operands, completed GPU acknowledgement and cleanup, no timed readback. MPSGraph is a benchmark-only numerical-library reference; it need not use Kalandra’s exact product/reduction order or resource/cancellation contract. Both satisfy the independent propagated numerical bounds. This is not an equivalent scalar-source language benchmark.

Twenty-four full-output checks pass across odd, small, 64-token and 512-token shapes, two input patterns and the production/MPSGraph/counter paths. The counter copy is bit-identical to production for every checked output.

## Matched uninstrumented library comparison

Three adjacent ABBA/BAAB process blocks, matching iterations per pair, two warmups, three measured rounds, 500 ms adaptive target and 8,192-call ceiling. Both graph/pipeline setup and initial resident uploads are excluded. AA uses the identical current Kalandra executable on both sides. Larger runtime/MPSGraph ratios mean a larger library gap. No samples are removed.

| Study | Tokens × width | Kalandra / reference time | Block range | Control precision |
|---|---|---:|---:|---|
| aa | 1 × 512 | 0.868× | 0.850–1.069× | fail |
| aa | 64 × 512 | 1.041× | 0.900–1.178× | fail |
| aa | 512 × 512 | 1.005× | 0.999–1.008× | pass |
| library | 1 × 512 | 1.237× | 1.178–1.393× | — |
| library | 64 × 512 | 1.732× | 1.091–1.757× | — |
| library | 512 × 512 | 2.808× | 2.623–2.855× | — |
| repeat | 1 × 512 | 1.213× | 1.005–1.304× | — |
| repeat | 64 × 512 | 1.626× | 1.267–1.691× | — |
| repeat | 512 × 512 | 2.798× | 2.482–2.847× | — |

## Separate encoder diagnosis

Counter sampling changes execution overhead. Intervals include stalls/scheduling and their sum is not an exclusive wall-time decomposition. Three processes per shape execute 128 warmups and 192 measured calls. These timings are never used as library acceptance.

| Tokens | Encoder | Median interval µs | Process range µs |
|---|---|---:|---:|
| 64 | FFN contraction + residual | 135.77 | 98.85–294.10 |
| 64 | FFN expansion + ReLU | 130.96 | 92.75–279.37 |
| 64 | Probabilities × V | 81.29 | 57.04–174.92 |
| 64 | V projection | 66.94 | 47.92–145.10 |
| 64 | K projection | 64.71 | 30.50–89.44 |
| 64 | Q projection | 64.10 | 51.67–158.62 |
| 64 | Output projection + residual | 40.92 | 29.21–84.71 |
| 64 | K transpose | 28.44 | 20.04–62.52 |
| 64 | QKᵀ + scale | 26.69 | 19.58–55.06 |
| 64 | Softmax | 14.67 | 10.77–30.04 |
| 64 | Attention RMSNorm | 13.81 | 10.33–27.62 |
| 64 | FFN RMSNorm | 12.12 | 8.88–25.50 |
| 512 | FFN contraction + residual | 632.44 | 631.94–633.17 |
| 512 | FFN expansion + ReLU | 616.19 | 616.06–616.35 |
| 512 | K projection | 319.29 | 317.10–320.23 |
| 512 | Q projection | 318.69 | 316.96–319.77 |
| 512 | V projection | 261.40 | 242.52–273.54 |
| 512 | Output projection + residual | 169.12 | 167.88–170.12 |
| 512 | QKᵀ + scale | 164.10 | 161.52–167.25 |
| 512 | Probabilities × V | 160.92 | 160.71–161.56 |
| 512 | K transpose | 76.44 | 74.77–94.73 |
| 512 | Softmax | 42.02 | 41.29–42.06 |
| 512 | Attention RMSNorm | 22.29 | 22.25–22.52 |
| 512 | FFN RMSNorm | 21.46 | 21.38–21.71 |

## Strict wider-tile experiments

Both use the production-aligned 2D grid and exact balanced products/sums. Pair32 reuses A across two output columns; the second experiment halves K panels to reduce shared/private live resources while retaining all carry roots. Each passes 84 full-output comparisons including non-dyadic finite data, IEEE patterns, signed zero, non-power-of-two chunk counts, unchanged inputs and output guard bytes. These are isolated preallocated-kernel screens, not managed whole-model speedups. Both are rejected.

| Tile | M × K × N | Median time reduction | Block range |
|---|---|---:|---:|
| gpu-wide-grid | 16 × 512 × 512 | -75.91% | -81.78% … -72.08% |
| gpu-wide-grid | 64 × 512 × 512 | -32.25% | -34.90% … -30.05% |
| gpu-wide-grid | 64 × 512 × 2048 | -46.42% | -47.00% … -46.16% |
| gpu-wide-grid | 64 × 2048 × 512 | -37.47% | -37.62% … -36.37% |
| gpu-wide-grid | 128 × 512 × 512 | -36.73% | -38.77% … -36.29% |
| gpu-wide-grid | 128 × 512 × 2048 | -44.24% | -45.18% … -43.07% |
| gpu-wide-grid | 128 × 2048 × 512 | -39.32% | -40.72% … -38.92% |
| gpu-wide-grid | 512 × 512 × 512 | -52.95% | -53.16% … -51.10% |
| gpu-wide-grid | 512 × 512 × 2048 | -45.35% | -47.26% … -44.49% |
| gpu-wide-grid | 512 × 2048 × 512 | -62.04% | -63.38% … -61.83% |
| gpu-wide-grid-leaf32 | 16 × 512 × 512 | -88.28% | -92.84% … -86.86% |
| gpu-wide-grid-leaf32 | 64 × 512 × 512 | -44.57% | -46.39% … -41.41% |
| gpu-wide-grid-leaf32 | 64 × 512 × 2048 | -60.57% | -61.79% … -57.33% |
| gpu-wide-grid-leaf32 | 64 × 2048 × 512 | -51.47% | -51.95% … -49.57% |
| gpu-wide-grid-leaf32 | 128 × 512 × 512 | -49.22% | -53.48% … -48.81% |
| gpu-wide-grid-leaf32 | 128 × 512 × 2048 | -56.49% | -57.63% … -55.70% |
| gpu-wide-grid-leaf32 | 128 × 2048 × 512 | -53.45% | -54.05% … -52.62% |
| gpu-wide-grid-leaf32 | 512 × 512 × 512 | -68.04% | -68.54% … -67.09% |
| gpu-wide-grid-leaf32 | 512 × 512 × 2048 | -57.60% | -59.47% … -56.05% |
| gpu-wide-grid-leaf32 | 512 × 2048 × 512 | -76.77% | -77.26% … -75.39% |

The first screen has no external-load record: sampler PID discovery failed and the verified PID had exited by the second attachment. That limitation is explicitly retained. The second screen records load from process launch; its context string inherited an old cache-study label, while its actual command/source/artifact identities identify this isolated kernel screen. Neither is promoted or used as proof of an uncontended production regression.

The matched 512-token library gap and dominant FFN intervals point to matrix computation, not more invocation bookkeeping. Failed layouts do not prove every strict kernel strategy is exhausted, or that register pressure alone caused slowdowns. No rounding relaxation or production vendor dependency is introduced.
