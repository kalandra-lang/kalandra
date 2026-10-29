# Invocation-private GPU parameter arena

Compared with frozen `fb85c832` on Apple M2 Max, 29 October 2026. Lower elapsed time is better. This change shares argument backing within an invocation; shader arithmetic, work limits and command scheduling stay unchanged.

Primary resident medians improve 3.0–9.6%; 26 of 27 shape/mode medians and 119 of 135 paired blocks improve. The 16×512 all-upload primary median is 0.19% slower and remains included; its control and repeat medians improve 1.25% and 1.92%. Independent resident replication ranges from 0.49% to 10.73%, so the smaller gains should not be treated as precise constants.

## Primary paired study

Five ABBA/BAAB blocks per shape and residency mode, fresh processes, two warmups and three measured batches, adaptive batches of at least 30 ms. Timings include invocation, selected fresh owned uploads and cleanup, excluding readback and cold preparation. All outcomes are retained.

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +7.52% (5/5 positive) | +8.47% (5/5 positive) | +7.30% (5/5 positive) |
| 16 × 64 | +8.40% (5/5 positive) | +10.04% (5/5 positive) | +7.61% (5/5 positive) |
| 64 × 64 | +7.21% (5/5 positive) | +7.29% (3/5 positive) | +6.29% (5/5 positive) |
| 1 × 256 | +9.59% (5/5 positive) | +7.81% (5/5 positive) | +2.38% (5/5 positive) |
| 16 × 256 | +7.02% (5/5 positive) | +5.49% (5/5 positive) | +3.44% (5/5 positive) |
| 64 × 256 | +3.01% (5/5 positive) | +2.65% (3/5 positive) | +2.47% (4/5 positive) |
| 1 × 512 | +8.38% (5/5 positive) | +9.17% (3/5 positive) | +3.79% (4/5 positive) |
| 16 × 512 | +4.29% (4/5 positive) | +4.17% (5/5 positive) | -0.19% (1/5 positive) |
| 64 × 512 | +3.60% (5/5 positive) | +1.99% (4/5 positive) | +1.02% (3/5 positive) |

## Same-binary policy control

Only `parameterArena` changes in the same-binary control; the independent repeat uses fresh processes with the same frozen artifacts.

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +9.75% (3/3 positive) | +9.75% (3/3 positive) | +6.83% (3/3 positive) |
| 16 × 64 | +6.90% (3/3 positive) | +9.08% (3/3 positive) | +7.18% (3/3 positive) |
| 64 × 64 | +7.49% (3/3 positive) | +8.15% (3/3 positive) | +8.03% (3/3 positive) |
| 1 × 256 | +8.07% (3/3 positive) | +3.04% (2/3 positive) | +6.56% (3/3 positive) |
| 16 × 256 | +5.07% (3/3 positive) | +6.04% (3/3 positive) | +3.14% (3/3 positive) |
| 64 × 256 | +3.66% (3/3 positive) | +3.75% (3/3 positive) | +3.54% (3/3 positive) |
| 1 × 512 | +4.73% (3/3 positive) | +8.79% (3/3 positive) | +1.57% (2/3 positive) |
| 16 × 512 | +2.70% (2/3 positive) | +4.54% (3/3 positive) | +1.25% (2/3 positive) |
| 64 × 512 | +1.04% (3/3 positive) | +1.36% (3/3 positive) | +1.44% (3/3 positive) |

## Independent replication

Only `parameterArena` changes in the same-binary control; the independent repeat uses fresh processes with the same frozen artifacts.

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +10.09% (5/5 positive) | +7.96% (5/5 positive) | +7.14% (5/5 positive) |
| 1 × 512 | +10.73% (5/5 positive) | +5.45% (5/5 positive) | +1.28% (3/5 positive) |
| 16 × 512 | +0.49% (4/5 positive) | +2.84% (5/5 positive) | +1.92% (4/5 positive) |
| 64 × 512 | +1.70% (3/5 positive) | +2.71% (5/5 positive) | +2.47% (4/5 positive) |

## Fresh CPU and MPSGraph comparison

Process-median microseconds from five shuffled rounds. Absolute medians drift between studies; use the paired and control measurements to attribute the gain from this change. This is a single unmasked, single-head, bias-free pre-RMSNorm block with a 4× ReLU FFN, not full LLM generation. MPSGraph is only a benchmark reference.

| Tokens × width | Mode | CPU µs | Metal µs | MPSGraph µs | Metal / MPS time |
|---|---|---:|---:|---:|---:|
| 1 × 64 | resident | 14.90 | 418.94 | 320.26 | 1.31× |
| 1 × 64 | activation | 14.90 | 413.90 | 314.49 | 1.32× |
| 1 × 64 | all uploads | 14.90 | 484.71 | 397.39 | 1.22× |
| 16 × 64 | resident | 69.41 | 415.04 | 311.13 | 1.33× |
| 16 × 64 | activation | 69.41 | 415.09 | 337.95 | 1.23× |
| 16 × 64 | all uploads | 69.41 | 490.27 | 402.39 | 1.22× |
| 64 × 64 | resident | 265.99 | 485.94 | 328.95 | 1.48× |
| 64 × 64 | activation | 265.99 | 540.47 | 341.70 | 1.58× |
| 64 × 64 | all uploads | 265.99 | 572.78 | 415.65 | 1.38× |
| 1 × 256 | resident | 59.13 | 437.01 | 332.91 | 1.31× |
| 1 × 256 | activation | 59.13 | 454.62 | 316.67 | 1.44× |
| 1 × 256 | all uploads | 59.13 | 774.89 | 677.27 | 1.14× |
| 16 × 256 | resident | 1191.58 | 570.17 | 387.55 | 1.47× |
| 16 × 256 | activation | 1191.58 | 583.66 | 399.54 | 1.46× |
| 16 × 256 | all uploads | 1191.58 | 915.44 | 743.24 | 1.23× |
| 64 × 256 | resident | 2805.21 | 1041.50 | 441.11 | 2.36× |
| 64 × 256 | activation | 2805.21 | 1046.78 | 454.96 | 2.30× |
| 64 × 256 | all uploads | 2805.21 | 1377.86 | 792.73 | 1.74× |
| 1 × 512 | resident | 270.58 | 414.14 | 327.81 | 1.26× |
| 1 × 512 | activation | 270.58 | 408.40 | 331.25 | 1.23× |
| 1 × 512 | all uploads | 270.58 | 1874.94 | 1768.30 | 1.06× |
| 16 × 512 | resident | 5295.37 | 953.38 | 478.80 | 1.99× |
| 16 × 512 | activation | 5295.37 | 1001.37 | 474.73 | 2.11× |
| 16 × 512 | all uploads | 5295.37 | 2496.21 | 1950.35 | 1.28× |
| 64 × 512 | resident | 11147.39 | 1571.88 | 538.02 | 2.92× |
| 64 × 512 | activation | 11147.39 | 1583.39 | 561.45 | 2.82× |
| 64 × 512 | all uploads | 11147.39 | 3081.16 | 1988.82 | 1.55× |

## Host costs and rejected alternatives

The preceding baseline diagnostic profile used the [Mach clock shared by Metal GPU timestamps](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime). Inclusive buckets overlap; do not sum them as exclusive time. Median diagnostic microseconds per invocation:

| Shape | Wall | Buffer allocation | Buffer destruction | Command creation | Submit → GPU start | GPU end → callback | GPU intervals |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 × 64 | 420.43 | 53.24 | 25.99 | 0.52 | 91.79 | 70.80 | 152.05 |
| 1 × 512 | 421.11 | 49.82 | 25.12 | 0.49 | 93.14 | 69.91 | 155.11 |
| 16 × 512 | 966.87 | 69.54 | 30.05 | 1.23 | 275.91 | 235.43 | 326.08 |
| 64 × 512 | 1543.39 | 80.85 | 31.58 | 2.17 | 490.79 | 379.34 | 511.62 |

The largest baseline spends approximately 0.49 ms between submission and GPU start, and 0.38 ms between GPU end and callback entry. Allocation is much smaller, at 0.08 ms. These are measured intervals, not proof of a particular driver implementation.

Three benchmark-only replacements used a global default-priority worker, a global interactive worker, or a serial initiated-priority worker, waiting for actual completion before checking cancellation and admitting the next chunk. None provides a reliable win across both shapes; no completion worker change ships. Their per-block time reductions:

| Worker | 1 × 512 | 64 × 512 |
|---|---:|---:|
| global-default | -6.56%, -1.17%, -2.12% | -13.06%, -1.41%, -2.48% |
| global-interactive | +2.85%, -2.17%, -0.08% | -13.18%, +0.50%, +0.63% |
| serial-initiated | -3.10%, -0.55%, -1.42% | -0.27%, -1.86%, -2.35% |

## Correctness, ownership and scope

All Kalandra output cells match the frozen baseline across 13 regular/odd shapes and all residency modes. Independent CPU/MPSGraph numerical bounds pass. All 23 shader text artifacts and CPU LLVM remain unchanged, and both GPU variants use the original Metal binaries. Acceptance timing disables profiling. Submission counts are identical in every timing sample.

Four-mode prepared-program qualification covers changing captures, eight concurrent calls, aliases, ranks 1/3/8, 5/9/65/513-stage plans, cost saturation, failed guards, checked casts, teardown, allocation failure and every held cancellation/deadline/device-failure boundary. Parameter backing drops from 16 buffers to one; allocation checkpoints drop 109→95 for the small block and 113→99 for the large block. The separate-buffer control still passes all ownership checks.

A dedicated boundary fixture verifies that padding does not consume the previous 64 KiB admission headroom: 67,584-byte budgets retain separate parameter buffers and the same progression, while 73,728-byte budgets admit the arena. Both return exact matching values, remain within budget and release all bytes. Twenty-six broader storage, arithmetic, pipeline, graph, ReLU, transform and numeric test groups also pass.

The initial prepared test failed because it explicitly expected sixteen live parameter buffers. It now checks one for the arena and sixteen for the control. The failure and successful reruns are retained; no numerical assertion was relaxed.

No global cache, mutable cross-invocation storage, checkpoint relaxation, detached submission thread or vendor numerical dependency is added. The arena uses fresh owned storage with distinct 256-byte-aligned writable slices. See the [storage contract](../../docs/design/gpu-parameter-arena.md).

Next: the dominant remaining gap is command handoff latency. The tested host-worker alternatives do not solve it. Further progress needs a separately qualified submission/checkpoint design; allocation savings alone cannot erase that gap.

## Reproduction and evidence

The diagnostic scripts were run against `fb85c832` before production edits and retain generated source and binary hashes. Run them from that baseline with the recorded frozen work-chunk artifacts. The acceptance runner consumes those same frozen directories plus the current four-mode prepared qualification; it verifies signatures before timing and has `paired`, `control`, `repeat` and `matched` studies.

```sh
python3 native/benchmarks/parameter_arena_acceptance.py \
  --acceptance-dir benchmarks/.build/gpu-work-chunks-2026-10-29/acceptance \
  --program-dir benchmarks/.build/gpu-attention-2026-10-29/acceptance \
  --prepared-dir benchmarks/.build/gpu-host-costs-2026-10-29/prepared \
  --build-dir benchmarks/.build/gpu-host-costs-2026-10-29/acceptance \
  --study paired --blocks 5 --output /tmp/arena-new-paired.json
```

- [gpu-parameter-arena-paired-2026-10-29.json](gpu-parameter-arena-paired-2026-10-29.json)
- [gpu-parameter-arena-control-2026-10-29.json](gpu-parameter-arena-control-2026-10-29.json)
- [gpu-parameter-arena-repeat-2026-10-29.json](gpu-parameter-arena-repeat-2026-10-29.json)
- [gpu-parameter-arena-matched-2026-10-29.json](gpu-parameter-arena-matched-2026-10-29.json)
- [gpu-parameter-arena-qualification-2026-10-29.json](gpu-parameter-arena-qualification-2026-10-29.json)
- [gpu-host-costs-profile-2026-10-29.json](gpu-host-costs-profile-2026-10-29.json)
- [gpu-host-completion-screen-2026-10-29.json](gpu-host-completion-screen-2026-10-29.json)
- [gpu-parameter-arena-screen-2026-10-29.json](gpu-parameter-arena-screen-2026-10-29.json)
- [gpu-parameter-arena-load-2026-10-29.json](gpu-parameter-arena-load-2026-10-29.json)
