# Contiguous GPU transpose views

Compared with frozen `aacae818` on Apple M2 Max, 29 October 2026. A singleton or empty transpose shares immutable backing with new shape metadata. General matrix kernels and arithmetic are unchanged.

The optimization applies to the one-row shapes in this study. Other shapes are retained as regression controls; small timing differences there are not evidence of a transpose-view speedup. Pure transpose chains additionally verify zero submissions and unchanged physical bytes.

One-row resident medians improve 0.37–3.07% in the primary run and 0.98–4.01% in replication; the additional frozen confirmation ranges from 0.01% to 2.53%. Treat these as modest improvements, especially the nearly flat 1×256 confirmation. Streaming medians improve 0.77–2.99%. The fresh largest resident block takes 1,560.30 µs: 7.04× faster than the own CPU path, but 2.85× MPSGraph time.

The 64×256 primary resident regression of 2.42% does not repeat against the frozen binary (+0.37%), while its same-binary confirmation moves −2.33%. The 64×64 all-upload controls remain negative (−1.01% to −5.27%). These shapes cannot use a view, so the evidence does not establish a mechanism or a blanket performance gain; every result is retained. Large positive control excursions (+21.97% on an inactive 64×512 case and +19.04% on 1×256 activation upload) are also retained and are not presented as expected gains.

## Paired results

Positive values mean less elapsed time. Five fresh-process ABBA/BAAB blocks for primary/repeat, three for initial policy/streaming controls and five for both confirmation studies; two warmups and three measured batches per process. Adaptive batches target at least 30 ms. Timings include complete invocation, selected owned uploads and cleanup, excluding readback and cold preparation.

### Frozen baseline

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +0.67% (5/5 positive) | +3.40% (5/5 positive) | +1.76% (4/5 positive) |
| 16 × 64 | +1.63% (5/5 positive) | +1.09% (3/5 positive) | -0.83% (2/5 positive) |
| 64 × 64 | +0.21% (3/5 positive) | -0.23% (2/5 positive) | -1.01% (2/5 positive) |
| 1 × 256 | +0.37% (3/5 positive) | +1.42% (3/5 positive) | +1.90% (3/5 positive) |
| 16 × 256 | +0.40% (3/5 positive) | -1.87% (1/5 positive) | -0.55% (2/5 positive) |
| 64 × 256 | -2.42% (0/5 positive) | -1.01% (2/5 positive) | -1.77% (2/5 positive) |
| 1 × 512 | +3.07% (4/5 positive) | +0.32% (4/5 positive) | -0.39% (1/5 positive) |
| 16 × 512 | -2.41% (1/5 positive) | +1.52% (4/5 positive) | +2.50% (4/5 positive) |
| 64 × 512 | -0.09% (2/5 positive) | -0.80% (2/5 positive) | +0.72% (3/5 positive) |

### Same-binary policy control

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +1.22% (3/3 positive) | +2.48% (3/3 positive) | -1.67% (1/3 positive) |
| 16 × 64 | -0.10% (1/3 positive) | +0.89% (2/3 positive) | +0.34% (2/3 positive) |
| 64 × 64 | +0.62% (2/3 positive) | -0.63% (1/3 positive) | -5.27% (0/3 positive) |
| 1 × 256 | +0.97% (3/3 positive) | +0.64% (2/3 positive) | +1.12% (3/3 positive) |
| 16 × 256 | +0.62% (2/3 positive) | +1.01% (3/3 positive) | +3.19% (2/3 positive) |
| 64 × 256 | +0.29% (2/3 positive) | +0.52% (2/3 positive) | -2.48% (0/3 positive) |
| 1 × 512 | +2.50% (3/3 positive) | +3.02% (3/3 positive) | +3.04% (3/3 positive) |
| 16 × 512 | +0.74% (2/3 positive) | +0.57% (2/3 positive) | +0.78% (3/3 positive) |
| 64 × 512 | +21.97% (2/3 positive) | -0.98% (1/3 positive) | +2.21% (3/3 positive) |

### Independent replication

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +2.94% (4/5 positive) | +0.78% (3/5 positive) | +0.82% (4/5 positive) |
| 1 × 256 | +0.98% (3/5 positive) | +2.58% (3/5 positive) | +0.62% (3/5 positive) |
| 1 × 512 | +4.01% (5/5 positive) | +0.40% (4/5 positive) | -0.40% (2/5 positive) |
| 64 × 512 | -1.08% (0/5 positive) | +2.42% (5/5 positive) | -1.32% (2/5 positive) |

### Streaming fallback control

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +1.31% (3/3 positive) | +2.99% (3/3 positive) | +2.16% (2/3 positive) |
| 1 × 512 | +0.91% (2/3 positive) | +0.77% (3/3 positive) | +1.80% (2/3 positive) |

### Frozen-baseline confirmation

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +2.53% (4/5 positive) | +1.19% (3/5 positive) | +1.34% (4/5 positive) |
| 16 × 64 | +0.53% (3/5 positive) | +0.51% (3/5 positive) | +0.69% (3/5 positive) |
| 64 × 64 | +1.32% (4/5 positive) | +1.42% (5/5 positive) | -2.93% (1/5 positive) |
| 1 × 256 | +0.01% (3/5 positive) | +19.04% (5/5 positive) | +1.07% (3/5 positive) |
| 16 × 256 | +0.69% (4/5 positive) | +0.76% (5/5 positive) | -0.13% (2/5 positive) |
| 64 × 256 | +0.37% (3/5 positive) | +0.56% (3/5 positive) | -1.03% (2/5 positive) |
| 1 × 512 | +1.32% (4/5 positive) | +2.96% (5/5 positive) | +0.98% (3/5 positive) |
| 16 × 512 | +0.28% (3/5 positive) | +0.15% (4/5 positive) | -0.16% (2/5 positive) |
| 64 × 512 | +0.61% (3/5 positive) | -0.46% (2/5 positive) | -1.06% (1/5 positive) |

### Same-binary confirmation

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +2.64% (4/5 positive) | +2.51% (5/5 positive) | +2.82% (5/5 positive) |
| 16 × 64 | -0.10% (2/5 positive) | +1.57% (4/5 positive) | +0.93% (4/5 positive) |
| 64 × 64 | +1.85% (4/5 positive) | -1.36% (2/5 positive) | -1.36% (1/5 positive) |
| 1 × 256 | +1.26% (3/5 positive) | +1.25% (3/5 positive) | +1.33% (4/5 positive) |
| 16 × 256 | -0.84% (1/5 positive) | +1.15% (3/5 positive) | +1.08% (4/5 positive) |
| 64 × 256 | -2.33% (0/5 positive) | +0.26% (3/5 positive) | -0.43% (2/5 positive) |
| 1 × 512 | +3.66% (4/5 positive) | +1.30% (3/5 positive) | +1.47% (3/5 positive) |
| 16 × 512 | -0.88% (2/5 positive) | -0.86% (2/5 positive) | -1.17% (2/5 positive) |
| 64 × 512 | +0.13% (4/5 positive) | +1.11% (3/5 positive) | -0.09% (2/5 positive) |

The policy control toggles only `contiguousTranspose` in the same binary. The streaming control also disables pre-encoding in both members. Removing a logical stage can shift fusion alignment in streaming: the tested one-row block retains 12 dispatches there, versus 11 in the normal work-bounded path. Command counts match between both members of every timed comparison.

## Fresh CPU and MPSGraph comparison

Five shuffled process rounds. Use paired controls to attribute changes; absolute timings drift between studies. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph is a benchmark reference only.

| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |
|---|---|---:|---:|---:|---:|
| 1 × 64 | resident | 14.91 | 407.17 | 303.30 | 1.34× |
| 1 × 64 | activation | 14.91 | 411.08 | 315.57 | 1.30× |
| 1 × 64 | all uploads | 14.91 | 482.04 | 383.72 | 1.26× |
| 16 × 64 | resident | 69.16 | 408.68 | 317.66 | 1.29× |
| 16 × 64 | activation | 69.16 | 418.26 | 326.00 | 1.28× |
| 16 × 64 | all uploads | 69.16 | 478.26 | 398.64 | 1.20× |
| 64 × 64 | resident | 268.35 | 484.58 | 316.64 | 1.53× |
| 64 × 64 | activation | 268.35 | 497.70 | 335.48 | 1.48× |
| 64 × 64 | all uploads | 268.35 | 567.81 | 424.36 | 1.34× |
| 1 × 256 | resident | 59.39 | 425.83 | 310.25 | 1.37× |
| 1 × 256 | activation | 59.39 | 439.07 | 314.40 | 1.40× |
| 1 × 256 | all uploads | 59.39 | 777.28 | 645.57 | 1.20× |
| 16 × 256 | resident | 1172.21 | 568.85 | 381.29 | 1.49× |
| 16 × 256 | activation | 1172.21 | 586.75 | 389.56 | 1.51× |
| 16 × 256 | all uploads | 1172.21 | 901.92 | 717.21 | 1.26× |
| 64 × 256 | resident | 2736.66 | 1005.06 | 438.57 | 2.29× |
| 64 × 256 | activation | 2736.66 | 1008.10 | 450.05 | 2.24× |
| 64 × 256 | all uploads | 2736.66 | 1342.70 | 772.46 | 1.74× |
| 1 × 512 | resident | 262.14 | 405.31 | 332.48 | 1.22× |
| 1 × 512 | activation | 262.14 | 414.69 | 327.43 | 1.27× |
| 1 × 512 | all uploads | 262.14 | 1820.95 | 1765.73 | 1.03× |
| 16 × 512 | resident | 4892.56 | 965.06 | 469.71 | 2.05× |
| 16 × 512 | activation | 4892.56 | 967.01 | 485.89 | 1.99× |
| 16 × 512 | all uploads | 4892.56 | 2435.22 | 1883.92 | 1.29× |
| 64 × 512 | resident | 10979.58 | 1560.30 | 547.79 | 2.85× |
| 64 × 512 | activation | 10979.58 | 1623.78 | 573.85 | 2.83× |
| 64 × 512 | all uploads | 10979.58 | 3079.07 | 1985.55 | 1.55× |

## Correctness and limits

All output cells match the frozen runtime across 13 regular/odd shapes and all residency modes. A separate 78-comparison check verifies both streaming policies against those same qualified outputs. Independent CPU/MPSGraph numerical bounds pass. Shader text, Metal libraries and CPU LLVM remain unchanged. Profiling and Metal validation are disabled during timing, and no tests or builds overlap acceptance. External CPU load is sampled and retained.

Four prepared-program modes cover exact bit payloads, nine-stage all-view programs, singleton/empty/general matrices, live aliases across differently shaped descriptors, original-input immutability, changing captures, eight concurrent calls, every injected allocation failure, every held cancellation/deadline/device-failure boundary, and teardown. Twenty-six additional runtime/numerical test groups pass, including transform coverage for all ten types.

The new streaming fixture initially failed its old expectation of four live parameter buffers at every boundary. Removing one logical transpose makes the final window contain three. The assertion now checks that exact count; numerical and cancellation assertions were not relaxed. Failed and successful logs are retained in qualification evidence.

No GPU copy or completion exists for an all-view path. Metadata allocation remains fallible; prior submitted work must still acknowledge before result publication. External values retain their non-reusable origin, and live aliases sharing the same physical allocation block destructive pointwise reuse. See the [ownership contract](../../docs/design/gpu-contiguous-transpose.md).

The [general transposed matrix profiles](gpu-transpose-matmul-profiles-2026-10-29.md) remain rejected because of larger-workload regressions. None of those kernels is included in this runtime change.

## Evidence and reproduction

Run `native/benchmarks/transpose_view_acceptance.py` with the frozen `gpu-host-costs-2026-10-29/acceptance` baseline, the `gpu-attention-2026-10-29/acceptance` shader/CPU artifacts, and current `gpu-transpose-view-2026-10-29/prepared` qualification. Its five studies are `paired`, `control`, `repeat`, `streaming`, and `matched`; every source/artifact digest is checked before timing.

- [gpu-transpose-view-paired-2026-10-29.json](gpu-transpose-view-paired-2026-10-29.json)
- [gpu-transpose-view-control-2026-10-29.json](gpu-transpose-view-control-2026-10-29.json)
- [gpu-transpose-view-repeat-2026-10-29.json](gpu-transpose-view-repeat-2026-10-29.json)
- [gpu-transpose-view-streaming-2026-10-29.json](gpu-transpose-view-streaming-2026-10-29.json)
- [gpu-transpose-view-matched-2026-10-29.json](gpu-transpose-view-matched-2026-10-29.json)
- [gpu-transpose-view-qualification-2026-10-29.json](gpu-transpose-view-qualification-2026-10-29.json)
- [gpu-transpose-view-load-2026-10-29.json](gpu-transpose-view-load-2026-10-29.json)
- [gpu-transpose-view-streaming-verification-2026-10-29.json](gpu-transpose-view-streaming-verification-2026-10-29.json)
- [gpu-transpose-view-confirm-paired-2026-10-29.json](gpu-transpose-view-confirm-paired-2026-10-29.json)
- [gpu-transpose-view-confirm-control-2026-10-29.json](gpu-transpose-view-confirm-control-2026-10-29.json)
- [gpu-transpose-view-confirmation-load-2026-10-29.json](gpu-transpose-view-confirmation-load-2026-10-29.json)
