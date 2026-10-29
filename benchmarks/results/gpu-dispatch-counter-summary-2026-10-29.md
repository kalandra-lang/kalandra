# Diagnostic GPU encoder intervals

Apple M2 Max. A copied runtime samples Metal timestamps at each encoder boundary. Production shaders, arithmetic and dispatch geometry are unchanged; eight full output comparisons pass under Metal validation. Sampling is instrumentation, so these numbers are not performance acceptance and must not replace the unprofiled paired report.

Five fresh processes per shape, two warmups and seven measured calls per process. Tables use the median of process medians. Each call contains one block. GPU timestamps are calibrated to CPU nanoseconds using paired device timestamps around the command. Encoder intervals are inclusive and may contain synchronization and scheduling costs; they are not exclusive ALU time or an additive wall-time decomposition.

| Tokens × width | Command GPU µs | Sum of inclusive intervals µs | Instrumented wall µs |
|---|---:|---:|---:|
| 1 × 64 | 390.62 | 498.67 | 675.00 |
| 1 × 512 | 406.38 | 468.08 | 716.33 |
| 16 × 512 | 733.13 | 890.50 | 1105.04 |
| 64 × 512 | 1346.79 | 1603.96 | 1757.75 |

## 1 × 64

| Instruction | Encoder work | Median µs | Process-median range µs |
|---:|---|---:|---:|
| 32 | FFN contraction + residual | 75.04 | 28.62–75.38 |
| 24 | Output projection + residual | 59.29 | 21.04–59.83 |
| 11 | Q projection | 58.50 | 20.67–58.58 |
| 29 | FFN expansion + ReLU | 58.21 | 20.38–58.42 |
| 13 | K projection | 57.54 | 19.79–58.38 |
| 18 | QKᵀ + scale | 57.04 | 20.33–58.58 |
| 15 | V projection | 56.79 | 19.50–57.17 |
| 9 | Attention RMSNorm | 20.46 | 7.62–20.54 |
| 22 | Probabilities × V | 20.08 | 7.08–20.33 |
| 27 | FFN RMSNorm | 18.46 | 6.67–18.79 |
| 20 | Softmax | 15.67 | 6.04–16.00 |

## 1 × 512

| Instruction | Encoder work | Median µs | Process-median range µs |
|---:|---|---:|---:|
| 18 | QKᵀ + scale | 95.79 | 32.17–96.79 |
| 32 | FFN contraction + residual | 87.29 | 31.29–87.54 |
| 29 | FFN expansion + ReLU | 40.71 | 16.67–41.04 |
| 13 | K projection | 39.04 | 15.58–42.83 |
| 11 | Q projection | 38.79 | 15.67–43.58 |
| 24 | Output projection + residual | 35.71 | 13.54–35.79 |
| 15 | V projection | 32.71 | 11.58–32.75 |
| 22 | Probabilities × V | 27.25 | 9.58–27.58 |
| 9 | Attention RMSNorm | 26.88 | 10.08–27.29 |
| 27 | FFN RMSNorm | 23.62 | 8.04–23.75 |
| 20 | Softmax | 15.67 | 6.25–15.88 |

## 16 × 512

| Instruction | Encoder work | Median µs | Process-median range µs |
|---:|---|---:|---:|
| 32 | FFN contraction + residual | 266.54 | 93.33–267.50 |
| 29 | FFN expansion + ReLU | 102.33 | 35.08–105.67 |
| 11 | Q projection | 86.96 | 31.54–89.33 |
| 13 | K projection | 85.21 | 30.08–89.71 |
| 24 | Output projection + residual | 84.83 | 32.04–87.17 |
| 15 | V projection | 80.92 | 26.79–82.04 |
| 22 | Probabilities × V | 50.38 | 16.96–50.62 |
| 18 | QKᵀ + scale | 36.96 | 12.92–37.21 |
| 9 | Attention RMSNorm | 27.50 | 10.33–27.58 |
| 20 | Softmax | 25.67 | 9.33–26.00 |
| 27 | FFN RMSNorm | 24.58 | 8.50–24.88 |
| 16 | K transpose | 17.12 | 6.04–17.25 |

## 64 × 512

| Instruction | Encoder work | Median µs | Process-median range µs |
|---:|---|---:|---:|
| 32 | FFN contraction + residual | 342.38 | 119.62–343.62 |
| 29 | FFN expansion + ReLU | 335.50 | 114.50–337.83 |
| 11 | Q projection | 183.00 | 35.75–187.88 |
| 22 | Probabilities × V | 175.21 | 59.42–175.50 |
| 15 | V projection | 159.83 | 46.08–164.00 |
| 13 | K projection | 103.75 | 62.96–105.33 |
| 24 | Output projection + residual | 102.54 | 37.75–103.12 |
| 18 | QKᵀ + scale | 70.83 | 20.29–75.00 |
| 16 | K transpose | 45.62 | 23.92–46.96 |
| 20 | Softmax | 30.00 | 11.08–30.17 |
| 9 | Attention RMSNorm | 27.79 | 10.62–28.04 |
| 27 | FFN RMSNorm | 25.42 | 9.00–25.58 |

## Interpretation limits

The two FFN matrix passes account for 42.1–42.5% of the sum of per-process median encoder intervals at 64 × 512. They remain the leading combined target in every process. This fraction is not their share of total wall time.

Absolute intervals vary by roughly threefold between early and later processes. These short instrumented bursts do not control GPU frequency, competing device activity or counter-induced scheduling. The data cannot assign a cause to that drift; establish stable sustained-load profiling before accepting a kernel speedup. Pipeline labels are unavailable (`unnamed`); operation names come from instruction positions in the frozen generated program.

Use the largest stable encoder intervals to choose a kernel experiment. These counters alone do not establish memory bandwidth, occupancy, arithmetic throughput or why MPSGraph differs. Fused epilogues are included in their producer. The one-row K transpose is an existing zero-copy view and has no encoder. Keep strict arithmetic order and qualify all output bits before any candidate is considered for production.

Timestamp conversion follows [Apple's GPU/CPU time guidance](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time). Raw samples, pipeline-label fields, source hashes and output checks are retained in the input JSON.
