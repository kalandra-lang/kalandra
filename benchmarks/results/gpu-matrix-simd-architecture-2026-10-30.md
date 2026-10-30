# SIMD-group matrix architecture screen — 30 October 2026

These own-code Metal prototypes are isolated preallocated kernels. Neither is selected by production, and neither establishes a whole-model MPSGraph speedup. The strict production baseline remains `8814a901`. See the [matched library investigation](gpu-kernel-library-gap-2026-10-30.md).

The naive prototype uses one SIMD group per 8×8 output tile. Block32 stages 32×32 panels in 8 KiB of threadgroup memory; four SIMD groups each maintain four 8×8 accumulators. Both use the Metal SIMD-group matrix primitive, without a vendor numerical library. This does not establish which physical arithmetic units execute it.

## Numerical boundary

Each prototype passes 20 cases: five shapes, ordinary finite or negative-zero inputs, and both implementations. Each case checks every cell in three rounds, unchanged inputs and output suffix guards. The strict baseline matches the balanced CPU arithmetic tree bit for bit. Candidate finite values satisfy a double-precision dot-product reference bound of gamma(K) × sum(abs(products)), where gamma(K) = K×2^-24 / (1−K×2^-24), plus 2^-100. This conservative bound does not establish a new language-wide numerical contract.

Candidates change rounding and turn tested negative zeros into positive zeros. NaN, infinity, overflow, subnormal behavior, arbitrary dimensions and production cancellation/resource integration are not qualified here. They cannot silently replace the existing strict matmul.

| Prototype | M × K × N | Baseline µs | Candidate µs | Speedup | Block range |
|---|---|---:|---:|---:|---:|
| gpu-matrix-simd | 64 × 512 × 2048 | 88.88 | 71.48 | 1.243× | 1.226–1.255× |
| gpu-matrix-simd | 64 × 2048 × 512 | 94.87 | 75.12 | 1.265× | 1.260–1.269× |
| gpu-matrix-simd | 512 × 512 × 512 | 160.39 | 131.50 | 1.217× | 1.212–1.228× |
| gpu-matrix-simd | 512 × 512 × 2048 | 611.14 | 494.95 | 1.235× | 1.230–1.239× |
| gpu-matrix-simd | 512 × 2048 × 512 | 625.59 | 517.15 | 1.210× | 1.207–1.212× |
| gpu-matrix-simd-block32 | 64 × 512 × 2048 | 89.32 | 46.59 | 1.921× | 1.895–1.970× |
| gpu-matrix-simd-block32 | 64 × 2048 × 512 | 94.98 | 138.51 | 0.684× | 0.681–0.692× |
| gpu-matrix-simd-block32 | 512 × 512 × 512 | 160.71 | 62.56 | 2.575× | 2.554–2.627× |
| gpu-matrix-simd-block32 | 512 × 512 × 2048 | 611.11 | 217.92 | 2.804× | 2.794–2.812× |
| gpu-matrix-simd-block32 | 512 × 2048 × 512 | 623.56 | 235.26 | 2.656× | 2.645–2.665× |

Five fresh adjacent ABBA/BAAB blocks per shape, two warmups and three measured rounds, identical iterations within each pair, 100 ms adaptive target and 256-call ceiling. Values are elapsed time per invocation, not instrumented encoder time. All samples and load records are retained; there is no separate identical-build control for these exploratory screens.

Block32 substantially improves the large FFN kernels, but regresses the 64×2048×512 contraction. A universal shape policy is not justified. Do not multiply these speedups into model throughput: other kernels, epilogues, synchronization and runtime work remain.

## Retained setup failures and next decision

The first two attempts selected dispatch geometry using pipeline object identity. A separately created strict baseline pipeline therefore received 32 threads instead of 256 and failed output validation. The corrected geometry uses tile width and checks each pipeline’s capacity. Accuracy bounds were not loosened. Both failure logs remain beside these results.

The load recorder context strings inherit an older study label; actual command lines and source/artifact hashes identify these experiments. No production source, numerical or cancellation contract changes in this investigation.

Next: choose whether to expose an explicit optimized matrix operation while keeping strict matmul. Before promotion, define exceptional value/precision behavior, qualify boundaries and arbitrary shapes, preserve fused epilogues, and run independent full-model numerical checks plus frozen paired/control/MPSGraph measurements.
