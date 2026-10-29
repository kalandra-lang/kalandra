# Overlap encoding with bounded GPU work

Compared with frozen `7ebdeb49` on Apple M2 Max, 29 October 2026. The CPU can encode later commands while the first command runs. One command per invocation remains in flight; shader arithmetic, work limits and acknowledgement/cancellation boundaries are unchanged.

Only multi-command, cost-proven plans with sufficient resource headroom use overlap. Single-command cases use the ordinary-submit fast path instead of the pre-encoded chain; they do not overlap encoding. Streaming remains a policy control. Every outcome, including regressions, is retained. See the [ownership and admission design](../../docs/design/gpu-encoding-overlap.md).

## Outcome

Largest resident time falls 4.17% in the primary study, 3.90% in replication and 3.95% in longer-batch confirmation; same-binary controls give 4.87% and 3.59%. Largest all-upload reductions are 3.46%, 3.76% and 2.13%. Activation-upload confirmation is negative (-1.19%) despite positive earlier/control results. Smaller cases are mixed: longer frozen-baseline resident controls range down to -3.02%, while corresponding same-binary medians are much closer to zero. No blanket improvement or regression-free claim is made. The original -15.31% 16×256 activation result is retained; its longer confirmation is -1.12%.

Fresh largest resident medians are CPU 11.019 ms, Metal 1.516 ms and MPSGraph 0.546 ms: 7.27× our CPU speed and 2.78× MPSGraph time. The handoff/device gap remains substantial.

## Paired results

Positive values mean less elapsed time. Five fresh-process ABBA/BAAB blocks in primary/repeat studies and three in policy/streaming controls. Two warmups and three measured batches per process; adaptive batches target at least 30 ms. Confirmation retains all 27 cases with seven frozen-baseline and five same-binary blocks at a 100 ms target, using exactly the same executables and inputs. Timings include complete invocation, selected owned uploads and cleanup, excluding readback and cold preparation.

### Frozen baseline

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +0.34% (3/5 positive) | +0.21% (3/5 positive) | +1.21% (4/5 positive) |
| 16 × 64 | +1.48% (4/5 positive) | +0.13% (3/5 positive) | +1.76% (5/5 positive) |
| 64 × 64 | -0.61% (2/5 positive) | +0.58% (3/5 positive) | +2.55% (4/5 positive) |
| 1 × 256 | +0.24% (3/5 positive) | +0.39% (3/5 positive) | -0.53% (1/5 positive) |
| 16 × 256 | +2.09% (3/5 positive) | -15.31% (2/5 positive) | +0.75% (3/5 positive) |
| 64 × 256 | +1.78% (5/5 positive) | +2.35% (4/5 positive) | +0.85% (3/5 positive) |
| 1 × 512 | -0.26% (2/5 positive) | -0.70% (2/5 positive) | +0.83% (3/5 positive) |
| 16 × 512 | -0.02% (2/5 positive) | +1.04% (4/5 positive) | +3.29% (4/5 positive) |
| 64 × 512 | +4.17% (5/5 positive) | +2.12% (3/5 positive) | +3.46% (5/5 positive) |

### Same-binary policy control

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +1.83% (3/3 positive) | -9.56% (0/3 positive) | +0.69% (3/3 positive) |
| 16 × 64 | +1.10% (2/3 positive) | -4.83% (0/3 positive) | +1.94% (3/3 positive) |
| 64 × 64 | -9.80% (1/3 positive) | -1.36% (1/3 positive) | +0.59% (2/3 positive) |
| 1 × 256 | +1.68% (2/3 positive) | -12.44% (0/3 positive) | -1.50% (0/3 positive) |
| 16 × 256 | +0.98% (3/3 positive) | -1.61% (0/3 positive) | +1.28% (3/3 positive) |
| 64 × 256 | +2.09% (3/3 positive) | +4.58% (2/3 positive) | +2.23% (2/3 positive) |
| 1 × 512 | +0.67% (2/3 positive) | -3.17% (1/3 positive) | -2.48% (1/3 positive) |
| 16 × 512 | -0.90% (1/3 positive) | +2.13% (3/3 positive) | +2.36% (2/3 positive) |
| 64 × 512 | +4.87% (3/3 positive) | +5.06% (3/3 positive) | +7.91% (3/3 positive) |

### Independent replication

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 64 × 256 | +1.67% (4/5 positive) | -0.19% (2/5 positive) | -0.63% (1/5 positive) |
| 1 × 512 | -0.77% (2/5 positive) | +0.44% (3/5 positive) | +2.21% (4/5 positive) |
| 16 × 512 | +0.86% (5/5 positive) | +0.20% (4/5 positive) | +2.38% (3/5 positive) |
| 64 × 512 | +3.90% (5/5 positive) | +2.59% (5/5 positive) | +3.76% (5/5 positive) |

### Streaming fallback control

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +4.48% (3/3 positive) | +0.56% (3/3 positive) | +2.25% (3/3 positive) |
| 1 × 512 | +1.27% (2/3 positive) | +1.74% (2/3 positive) | +0.11% (2/3 positive) |

### Longer-batch frozen-baseline confirmation

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | -0.50% (3/7 positive) | +0.20% (4/7 positive) | -0.66% (3/7 positive) |
| 16 × 64 | -3.02% (1/7 positive) | +1.40% (6/7 positive) | -1.07% (3/7 positive) |
| 64 × 64 | -1.92% (3/7 positive) | +0.64% (5/7 positive) | +0.03% (4/7 positive) |
| 1 × 256 | -1.19% (2/7 positive) | -1.09% (2/7 positive) | -0.43% (2/7 positive) |
| 16 × 256 | -1.40% (2/7 positive) | -1.12% (2/7 positive) | -0.91% (2/7 positive) |
| 64 × 256 | +0.76% (4/7 positive) | +1.56% (5/7 positive) | -0.49% (3/7 positive) |
| 1 × 512 | -0.30% (3/7 positive) | -0.15% (3/7 positive) | +0.77% (4/7 positive) |
| 16 × 512 | +0.65% (4/7 positive) | +1.44% (5/7 positive) | +0.85% (4/7 positive) |
| 64 × 512 | +3.95% (6/7 positive) | -1.19% (2/7 positive) | +2.13% (5/7 positive) |

### Longer-batch policy confirmation

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +0.06% (3/5 positive) | +0.11% (3/5 positive) | +0.49% (3/5 positive) |
| 16 × 64 | -0.14% (2/5 positive) | +1.18% (3/5 positive) | +0.42% (3/5 positive) |
| 64 × 64 | -0.86% (2/5 positive) | +0.02% (3/5 positive) | -2.58% (2/5 positive) |
| 1 × 256 | -0.16% (2/5 positive) | -0.32% (2/5 positive) | -0.59% (1/5 positive) |
| 16 × 256 | -0.13% (2/5 positive) | -1.50% (2/5 positive) | +1.83% (3/5 positive) |
| 64 × 256 | +0.74% (3/5 positive) | +2.12% (4/5 positive) | +0.73% (3/5 positive) |
| 1 × 512 | +0.41% (4/5 positive) | +0.42% (5/5 positive) | +1.21% (3/5 positive) |
| 16 × 512 | +3.53% (4/5 positive) | +0.88% (4/5 positive) | +0.49% (3/5 positive) |
| 64 × 512 | +3.59% (5/5 positive) | +2.42% (4/5 positive) | +2.18% (5/5 positive) |

The policy control toggles only `overlappedEncoding` in the same binary. Streaming disables pre-encoding in both members and has a separate full numerical verification sweep; overlap cannot apply there. Submission counts are identical in every timed sample.

## Fresh CPU and MPSGraph comparison

Five shuffled process rounds. Use paired controls to attribute changes; absolute timings drift between studies. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph is a benchmark reference only.

| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |
|---|---|---:|---:|---:|---:|
| 1 × 64 | resident | 14.94 | 422.22 | 317.68 | 1.33× |
| 1 × 64 | activation | 14.94 | 416.95 | 316.04 | 1.32× |
| 1 × 64 | all uploads | 14.94 | 493.64 | 392.35 | 1.26× |
| 16 × 64 | resident | 69.17 | 415.15 | 328.25 | 1.26× |
| 16 × 64 | activation | 69.17 | 423.04 | 328.45 | 1.29× |
| 16 × 64 | all uploads | 69.17 | 501.16 | 398.24 | 1.26× |
| 64 × 64 | resident | 264.97 | 493.55 | 328.22 | 1.50× |
| 64 × 64 | activation | 264.97 | 510.36 | 336.35 | 1.52× |
| 64 × 64 | all uploads | 264.97 | 560.26 | 419.39 | 1.34× |
| 1 × 256 | resident | 58.82 | 450.21 | 316.84 | 1.42× |
| 1 × 256 | activation | 58.82 | 460.77 | 312.05 | 1.48× |
| 1 × 256 | all uploads | 58.82 | 777.24 | 676.37 | 1.15× |
| 16 × 256 | resident | 1173.72 | 586.91 | 393.66 | 1.49× |
| 16 × 256 | activation | 1173.72 | 591.42 | 403.41 | 1.47× |
| 16 × 256 | all uploads | 1173.72 | 938.30 | 716.32 | 1.31× |
| 64 × 256 | resident | 2748.15 | 1016.17 | 443.26 | 2.29× |
| 64 × 256 | activation | 2748.15 | 1032.16 | 453.81 | 2.27× |
| 64 × 256 | all uploads | 2748.15 | 1347.37 | 781.23 | 1.72× |
| 1 × 512 | resident | 271.78 | 410.17 | 327.60 | 1.25× |
| 1 × 512 | activation | 271.78 | 420.14 | 326.54 | 1.29× |
| 1 × 512 | all uploads | 271.78 | 1792.37 | 1757.16 | 1.02× |
| 16 × 512 | resident | 4881.07 | 980.73 | 463.98 | 2.11× |
| 16 × 512 | activation | 4881.07 | 992.81 | 487.05 | 2.04× |
| 16 × 512 | all uploads | 4881.07 | 2453.16 | 1882.39 | 1.30× |
| 64 × 512 | resident | 11018.65 | 1515.86 | 545.86 | 2.78× |
| 64 × 512 | activation | 11018.65 | 1548.78 | 580.98 | 2.67× |
| 64 × 512 | all uploads | 11018.65 | 3033.48 | 1985.75 | 1.53× |

## Correctness, ownership and resource limits

Every output cell matches the frozen runtime across 13 regular/odd shapes and all residency modes, in both normal and separately verified streaming execution. Independent CPU/MPSGraph numerical bounds pass. Shader text, Metal libraries and CPU LLVM remain unchanged. Profiling and Metal validation are disabled during timing, and no tests or builds overlap acceptance. External CPU load is sampled and retained. Ordinary background applications remain active; this is not a dedicated idle lab machine. Pairing, replication and longer batches reduce drift but do not remove all outliers.

Four prepared-program modes and 27 broader runtime/numerical groups pass. Coverage includes changing captures, concurrent calls, all injected allocation checkpoints, held cancellation/deadline/device-failure boundaries, aliases, ranks, long chains and teardown. The enabled large case has 107 allocation checkpoints versus 99 for the fully pre-encoded control; every new point is injected.

Deterministic gates compile instrumented copies of production code; no gate is present in shipping libraries. They pause the builder and callback independently and require the external lease and resources to remain live until both finish. The original prototype failed the callback-gap regression. The repaired transition handles acknowledgement and the final admission/cleanup decision under one lock, and passes all four runtime modes.

Each overlapping command owns separate parameter backing, avoiding CPU writes to an in-flight shared resource. A 15,732,736-byte budget keeps the original one-arena/five-command policy; 15,794,176 bytes allows five arenas and the same five commands. Charged peak payload is 14,733,312 versus 14,749,696 bytes. Outputs match exactly, both limits hold, and the original 64 KiB safety headroom remains part of admission. No cross-invocation cache or larger work quantum is added.

The production rebuild changed task-archive debug metadata. Stripping debug data produces identical archives, and both timed executables link the exact frozen task archive. Its digests and the equivalence check are recorded. An unused completion parameter was removed after the warning-as-error build caught it.

## Evidence and reproduction

`native/benchmarks/encoding_overlap_acceptance.py` consumes the frozen `gpu-transpose-view-2026-10-29/acceptance` predecessor, `gpu-attention-2026-10-29/acceptance` shaders/CPU reference, and current `gpu-encoding-overlap-2026-10-29/prepared` qualification. Studies are `paired`, `control`, `repeat`, `streaming` and `matched`; input source/artifact digests are checked before timing. Earlier prototype screens must be reproduced from their recorded source commits, not the later production source.

- [gpu-encoding-overlap-paired-2026-10-29.json](gpu-encoding-overlap-paired-2026-10-29.json)
- [gpu-encoding-overlap-control-2026-10-29.json](gpu-encoding-overlap-control-2026-10-29.json)
- [gpu-encoding-overlap-repeat-2026-10-29.json](gpu-encoding-overlap-repeat-2026-10-29.json)
- [gpu-encoding-overlap-streaming-2026-10-29.json](gpu-encoding-overlap-streaming-2026-10-29.json)
- [gpu-encoding-overlap-matched-2026-10-29.json](gpu-encoding-overlap-matched-2026-10-29.json)
- [gpu-encoding-overlap-qualification-2026-10-29.json](gpu-encoding-overlap-qualification-2026-10-29.json)
- [gpu-encoding-overlap-load-2026-10-29.json](gpu-encoding-overlap-load-2026-10-29.json)
- [gpu-encoding-overlap-confirm-paired-2026-10-29.json](gpu-encoding-overlap-confirm-paired-2026-10-29.json)
- [gpu-encoding-overlap-confirm-control-2026-10-29.json](gpu-encoding-overlap-confirm-control-2026-10-29.json)
- [gpu-encoding-overlap-confirmation-load-2026-10-29.json](gpu-encoding-overlap-confirmation-load-2026-10-29.json)
