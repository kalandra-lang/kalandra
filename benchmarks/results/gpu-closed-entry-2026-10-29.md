# Closed synchronous GPU entry

Apple M2 Max, 29 October 2026. Frozen baseline: `6e8e7999`. The new standalone entry privately owns its scheduler; only a proved pure region without independent task/external progress becomes one GPU command. Ordinary `TaskCore` callers retain cancellation checkpoints. See the [entry and ownership design](../../docs/design/gpu-closed-entry.md).

Largest resident elapsed time falls **43.32%** in the primary paired comparison, **43.38%** in replication and **42.79%** against the same-binary closed-but-bounded control.

Fresh resident medians: CPU **10.690 ms**, closed Metal **0.841 ms**, MPSGraph **0.536 ms**. Metal is **12.71×** our CPU speed and takes **1.57×** MPSGraph's time. This does not establish parity with MPSGraph or a blanket GPU advantage for small workloads.

Small one-command cases show no consistent gain: the worst primary median is −1.60%, and the worst same-binary policy control is −4.02%. The ordinary cancellable API ranges from −1.85% to +1.38%; this is not an equivalence proof. All negative cells and block counts remain below.

## Paired changes and controls

Positive percentages mean less elapsed time; negative outcomes are retained. Fresh-process ABBA/BAAB blocks use two warmups and three measured batches per process, with adaptive batches targeting 100 ms. Invocation, selected owned uploads and cleanup are timed. Cold setup and readback are excluded. Counts of positive blocks expose consistency rather than hiding outliers.

Timed GPU iterations validate result shape, submitted work and resource cleanup. Their `checksum` field is a shape-derived sentinel, not a numerical reduction. Every output cell is checked in the separate untimed verification sweep; adding readback to each resident iteration would measure a different workload.

### Frozen cancellable entry → closed entry

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | -0.63% (3/7 positive) | +0.36% (4/7 positive) | +1.82% (5/7 positive) |
| 16 × 64 | +0.26% (6/7 positive) | -1.23% (3/7 positive) | -0.33% (3/7 positive) |
| 64 × 64 | -0.34% (2/7 positive) | -0.56% (3/7 positive) | -0.26% (3/7 positive) |
| 1 × 256 | +0.26% (4/7 positive) | -1.60% (3/7 positive) | -0.06% (3/7 positive) |
| 16 × 256 | -0.57% (2/7 positive) | +0.63% (4/7 positive) | -0.07% (2/7 positive) |
| 64 × 256 | +33.58% (7/7 positive) | +34.20% (7/7 positive) | +25.96% (7/7 positive) |
| 1 × 512 | +0.44% (4/7 positive) | -0.84% (3/7 positive) | +0.26% (4/7 positive) |
| 16 × 512 | +35.35% (7/7 positive) | +34.86% (7/7 positive) | +17.56% (7/7 positive) |
| 64 × 512 | +43.32% (7/7 positive) | +41.89% (7/7 positive) | +26.57% (7/7 positive) |

### Same binary: bounded closed owner → indivisible closed owner

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | -0.39% (2/5 positive) | -1.51% (1/5 positive) | -4.02% (1/5 positive) |
| 16 × 64 | -0.36% (2/5 positive) | -0.11% (2/5 positive) | -0.73% (2/5 positive) |
| 64 × 64 | +0.09% (3/5 positive) | -0.59% (2/5 positive) | -0.49% (2/5 positive) |
| 1 × 256 | -0.60% (1/5 positive) | +1.13% (3/5 positive) | -0.38% (1/5 positive) |
| 16 × 256 | +0.70% (4/5 positive) | +1.60% (5/5 positive) | -0.20% (2/5 positive) |
| 64 × 256 | +34.52% (5/5 positive) | +33.33% (5/5 positive) | +26.13% (5/5 positive) |
| 1 × 512 | -0.50% (2/5 positive) | +0.03% (3/5 positive) | +0.70% (3/5 positive) |
| 16 × 512 | +35.04% (5/5 positive) | +35.08% (5/5 positive) | +17.23% (5/5 positive) |
| 64 × 512 | +42.79% (5/5 positive) | +42.68% (5/5 positive) | +25.20% (3/5 positive) |

### Ordinary cancellable API: frozen → current

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +1.38% (4/5 positive) | -0.55% (2/5 positive) | +0.42% (3/5 positive) |
| 16 × 64 | +1.01% (4/5 positive) | +0.33% (3/5 positive) | -0.06% (2/5 positive) |
| 64 × 64 | -1.70% (2/5 positive) | +0.99% (3/5 positive) | +0.15% (3/5 positive) |
| 1 × 256 | +0.34% (5/5 positive) | -0.58% (1/5 positive) | -0.83% (2/5 positive) |
| 16 × 256 | -1.05% (1/5 positive) | -0.09% (1/5 positive) | +0.30% (4/5 positive) |
| 64 × 256 | -0.17% (2/5 positive) | -1.19% (0/5 positive) | -1.47% (1/5 positive) |
| 1 × 512 | +0.36% (3/5 positive) | +1.26% (3/5 positive) | -1.80% (2/5 positive) |
| 16 × 512 | -1.85% (1/5 positive) | +0.24% (3/5 positive) | -0.21% (2/5 positive) |
| 64 × 512 | +0.48% (3/5 positive) | -1.21% (1/5 positive) | -0.54% (2/5 positive) |

### Independent frozen-baseline replication

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 64 × 256 | +34.53% (5/5 positive) | +34.14% (5/5 positive) | +25.45% (5/5 positive) |
| 1 × 512 | +0.14% (3/5 positive) | -0.83% (2/5 positive) | +2.35% (4/5 positive) |
| 16 × 512 | +35.49% (5/5 positive) | +34.66% (5/5 positive) | +17.27% (5/5 positive) |
| 64 × 512 | +43.38% (5/5 positive) | +42.78% (5/5 positive) | +26.63% (5/5 positive) |

### Streaming fallback: both policies remain bounded

| Tokens × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +1.40% (3/3 positive) | +0.10% (2/3 positive) | +1.27% (2/3 positive) |
| 1 × 512 | +0.14% (2/3 positive) | +0.40% (2/3 positive) | +0.22% (2/3 positive) |

The closed policy control changes only `closedSynchronous` in one executable. The ordinary API comparison checks that the architectural addition did not shift its cost unnoticed. Streaming disables prepared progression and has a separate full output sweep. The primary comparison includes the new owner and its lifecycle; it is not a claim that cancellable task execution became indivisible.

## Fresh CPU and numerical-library comparison

Shuffled fresh processes; absolute values are measured separately from the paired experiment. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph remains a benchmark reference, not a production dependency.

| Tokens × width | Mode | CPU µs | Closed Metal µs | MPSGraph µs | Metal / MPS time |
|---|---|---:|---:|---:|---:|
| 1 × 64 | resident | 14.88 | 403.81 | 309.27 | 1.31× |
| 1 × 64 | activation | 14.88 | 407.15 | 313.06 | 1.30× |
| 1 × 64 | all uploads | 14.88 | 480.45 | 381.94 | 1.26× |
| 16 × 64 | resident | 68.58 | 418.23 | 306.24 | 1.37× |
| 16 × 64 | activation | 68.58 | 422.28 | 319.13 | 1.32× |
| 16 × 64 | all uploads | 68.58 | 482.63 | 386.16 | 1.25× |
| 64 × 64 | resident | 264.33 | 489.76 | 303.55 | 1.61× |
| 64 × 64 | activation | 264.33 | 473.55 | 328.75 | 1.44× |
| 64 × 64 | all uploads | 264.33 | 555.96 | 408.82 | 1.36× |
| 1 × 256 | resident | 58.49 | 439.22 | 306.41 | 1.43× |
| 1 × 256 | activation | 58.49 | 454.04 | 307.26 | 1.48× |
| 1 × 256 | all uploads | 58.49 | 753.81 | 619.48 | 1.22× |
| 16 × 256 | resident | 1163.33 | 571.72 | 351.11 | 1.63× |
| 16 × 256 | activation | 1163.33 | 580.35 | 391.14 | 1.48× |
| 16 × 256 | all uploads | 1163.33 | 888.42 | 704.22 | 1.26× |
| 64 × 256 | resident | 2716.84 | 659.94 | 430.28 | 1.53× |
| 64 × 256 | activation | 2716.84 | 676.85 | 445.50 | 1.52× |
| 64 × 256 | all uploads | 2716.84 | 972.65 | 753.70 | 1.29× |
| 1 × 512 | resident | 259.30 | 382.30 | 320.30 | 1.19× |
| 1 × 512 | activation | 259.30 | 411.11 | 326.28 | 1.26× |
| 1 × 512 | all uploads | 259.30 | 1752.33 | 1703.09 | 1.03× |
| 16 × 512 | resident | 4815.38 | 609.86 | 462.66 | 1.32× |
| 16 × 512 | activation | 4815.38 | 621.41 | 470.55 | 1.32× |
| 16 × 512 | all uploads | 4815.38 | 1984.44 | 1831.50 | 1.08× |
| 64 × 512 | resident | 10689.63 | 840.72 | 535.99 | 1.57× |
| 64 × 512 | activation | 10689.63 | 856.26 | 556.86 | 1.54× |
| 64 × 512 | all uploads | 10689.63 | 2170.16 | 1978.04 | 1.10× |

## Correctness and provenance

The native suite qualifies 1407 tests, with 0 failures. Dedicated runner and GPU checks cover all four build/sanitizer modes, output ownership, every allocation-failure point, device failures, source deadlines, independent siblings and missing-proof/long-plan fallback.

The initial full run exposed a late-observer race in the array-to-tensor cancellation fixture. A test-only gate now holds a real generated poll until its observer runs; ordinary arithmetic and production polling are unchanged. All four affected modes were rerun. Original failure logs remain in the qualification evidence, so the final coverage does not imply a clean first run.

Both LSP completion fixtures also assumed `tra` could suggest only `transfer`; the existing `transpose` builtin makes that prefix ambiguous. Updated assertions cover both candidates and a distinct `transf` prefix, including lexical shadowing. Both modes were rebuilt and rechecked without changing completion implementation.

Source, tensor-result and recovery fixtures assumed uploads always submitted blits. Shared uploads made their command ordinals target later work or wait for nonexistent submissions. They now arm a specific completion gate before execution and exercise both shared and private uploads. All 62 cases affected by fixture repairs were rechecked; production recovery behavior is unchanged.

Full output comparisons cover 13 regular/odd shapes and all upload modes. Frozen, ordinary, bounded, closed and streaming outputs match bit-for-bit; independent CPU/MPSGraph numerical bounds also pass. Metal shader text/libraries and CPU LLVM are unchanged. Accepted timings disable profiling and Metal validation, and do not overlap builds, tests or profiling.

Ordinary background applications remain active. External CPU-load samples are retained; this is not a dedicated idle lab machine. Pairing and replication reduce drift but do not erase all noise. The remaining device and dispatch gap needs separate profiling rather than arithmetic changes inferred from wall time alone.

An initial calibration attempt was stopped by the load guard when the Codex renderer briefly used about four CPU cores (555% total external CPU). Its load record is retained separately; no completed paired samples from that attempt enter this report. The frozen artifacts and full output verification were reused after the spike subsided.

## Evidence

- [paired](gpu-closed-entry-paired-2026-10-29.json)
- [control](gpu-closed-entry-control-2026-10-29.json)
- [ordinary](gpu-closed-entry-ordinary-2026-10-29.json)
- [repeat](gpu-closed-entry-repeat-2026-10-29.json)
- [streaming](gpu-closed-entry-streaming-2026-10-29.json)
- [matched](gpu-closed-entry-matched-2026-10-29.json)
- [qualification](gpu-closed-entry-qualification-2026-10-29.json)
- [load](gpu-closed-entry-load-2026-10-29.json)
- [interrupted-load](gpu-closed-entry-interrupted-load-2026-10-29.json)
