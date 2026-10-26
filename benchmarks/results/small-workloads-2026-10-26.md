# Small GPU workloads: direct scalar reduction output — 2026-10-26

The accepted change removes the final four-byte GPU blit from multi-element float32 reductions without scalar epilogues. The last compute dispatch writes directly into the frame-owned scalar readback buffer, or a fresh resident graph result. This removes a blit encoder and its GPU work, and skips final-level private storage when it would otherwise require a new buffer. Existing earlier-level scratch stays retained. There is no shader/arithmetic change, extra submission, vendor library, cache, cancellation opt-out or budget increase.

Physical measurements and qualification use Apple M2 Max/macOS 27.2, deployment target 26. Desktop applications remain active; no affinity, frequency or host isolation is imposed. Timing variation and losing blocks are retained. GPU/host profiling is separate from acceptance timing.

## Investigation and rejected alternative

The preceding grouped-traversal report measured resident 128² losses of 2.74–4.29% and resident 512² losses of 1.06–2.16%, although both sizes kept the original GPU entries. A five-block **identical-binary/identical-bundle A/A control** now records apparent reductions of −2.25% at 128², −0.02% at 512² and +0.37% at 2048². This demonstrates measurement variation of comparable magnitude; it does not prove all earlier regressions were noise or identify their physical cause.

A same-runtime old/new-bundle ablation removes the two optional grouped entries from the old bundle. It records −2.59% at 128², +0.01% at 512² and +4.67% at 2048². The last size changes the matrix entry and is expected to benefit; the smaller sizes keep their original entries. Small-case differences overlap A/A variation, so these runs do not establish a cold-loading/driver-cache explanation.

A strict adjacent-tree SIMD-shuffle prototype reduces the hardware reduction's seven workgroup barriers to one, retaining separate additions and odd-tail carries. It passes the initial numerical qualification, but its six-block complete-pipeline medians are +1.07% resident 128², approximately zero resident 512², −0.25% transfers 128² and −0.28% transfers 512², with wide mixed-sign ranges. **Rejected:** no reliable complete-pipeline benefit. The production shader header is restored byte-for-byte. The prototype header and initial runner source are preserved in the experiment record; ignored shader/binary artifacts are hashed by each raw timing record.

The accepted host change instead removes a known fixed cost that existed in both earlier versions. The paired result establishes its measured effect, not that the blit originally caused the grouping regression.

## Isolated host change against grouped-traversal baseline

Baseline is qualified commit `d61bde03dd980a2ff238fea26a986f3b9b028a7a`. Both binaries use the exact same frozen original shader bundle and benchmark driver/task-runtime archive; only the Metal runtime archive changes. Eight mirrored ABBA/BAAB blocks use equal calibrated iteration counts, three measured rounds and two full warmup batches per fresh process, targeting 40 ms per round. Every warmup/calibration/measured checksum must match the independent exact dyadic reference. The timer includes fresh intermediate allocation, scheduling, matrix/bias/scale/ReLU, reduction, scalar completion/readback and cleanup. Transfers additionally includes three uploads. Compilation, shader loading and resident initial uploads are excluded.

Positive percentages mean lower elapsed time. Percentages are medians of paired block ratios; ranges contain every observed block, not confidence intervals. The microsecond columns are medians of the per-block two-process means and need not reproduce the median paired ratio.

| Mode | Square N | Baseline µs | Direct output µs | Paired time reduction (full range) |
|---|---:|---:|---:|---:|
| resident | 128 | 294.55 | 265.09 | +7.94% (+1.79…+15.30%) |
| resident | 256 | 348.80 | 334.05 | +4.31% (+1.63…+5.57%) |
| resident | 512 | 492.22 | 478.66 | +3.10% (+1.42…+16.99%) |
| resident | 1024 | 1850.71 | 1819.40 | +1.60% (-1.97…+2.53%) |
| resident | 2048 | 12398.85 | 12390.68 | +0.13% (-1.79…+1.93%) |
| transfers | 128 | 786.64 | 768.12 | +2.77% (-0.68…+8.84%) |
| transfers | 256 | 895.66 | 873.91 | +2.43% (-3.45…+15.25%) |
| transfers | 512 | 1408.31 | 1394.67 | +1.45% (+0.15…+7.76%) |
| transfers | 1024 | 3870.47 | 3868.13 | -0.35% (-29.71…+3.55%) |
| transfers | 2048 | 18924.18 | 18826.70 | +0.15% (-8.10…+2.78%) |

Every resident 128²/256²/512² block improves. Transfers dilute the fixed-cost improvement with upload overhead and include losing blocks. The 1024² transfer median is −0.35%, including a −29.71% outlier; it is retained and is not a universal speedup claim. The 2048² median is essentially unchanged, preserving the grouped matrix path.

The benchmark's budget remains `max(64 MiB, 12*N*N*sizeof(float) + 4096)`, or 192 MiB + 4 KiB at 2048². The production default remains 64 MiB. The earlier report's statement of a 512 MiB benchmark limit was incorrect and is corrected there; that limit is used by correctness fixtures.

## Net confirmation against the pre-regression baseline

A second complete eight-block run compares the final host change plus existing grouping against qualified pre-grouping production (`5bf2a43ab5b314dcba66bff2ce18b106c7581b5d`). Driver, shader bundle, inputs and methodology remain matched. The old runtime ignores optional grouped entries; the new runtime selects grouping only under its existing policy. This comparison directly tests whether the earlier small-case losses are offset.

| Mode | Square N | Paired net time reduction (full range) |
|---|---:|---:|
| resident | 128 | +6.27% (+1.35…+11.79%) |
| resident | 256 | +5.36% (+2.57…+7.14%) |
| resident | 512 | +3.48% (+1.42…+6.00%) |
| resident | 1024 | +0.57% (-0.46…+1.84%) |
| resident | 2048 | +4.85% (+4.69…+5.37%) |
| transfers | 128 | +2.04% (-9.03…+5.00%) |
| transfers | 256 | +2.31% (-9.16…+7.49%) |
| transfers | 512 | +1.10% (-3.19…+2.77%) |
| transfers | 1024 | +0.59% (-14.92…+2.41%) |
| transfers | 2048 | +3.66% (-0.21…+5.19%) |

Every resident 128², 256² and 512² block improves, and every resident 2048² block retains the grouped-path gain. The earlier small resident regressions are offset in both accepted comparisons. Upload-inclusive results retain mixed-sign blocks; this is a measured improvement on this device, not a guarantee that every invocation or shape is faster. The 1024² transfer outliers remain visible in both records.

## Safety and compatibility

The final buffer is already allocated and owned by the task frame. It is distinct from every input and retained until GPU command acknowledgement. The result is published only after successful completion; cancellation/faults discard it through the existing cleanup path. One serial encoder orders reduction levels. Scratch/count preparation still finishes before opening that encoder, and all allocations retain budget reservations. Empty/singleton and scalar-epilogue copy paths remain intact. Older/software shader entries use the same host optimization. GPU kernels, public runtime header, matrix policy and task scheduler are unchanged.

## Separate profiling diagnostics

Four alternating-order fresh-process blocks per version/mode/size collect five rounds of ten iterations, with profiling enabled. These values are diagnostics, not the acceptance percentages above. Resident median host encoding and whole-command GPU durations are microseconds per call:

| Square N | Encoding, old → direct | GPU command, old → direct |
|---|---:|---:|
| 128 | 25.75 → 20.71 | 71.74 → 67.42 |
| 256 | 26.36 → 21.09 | 101.68 → 97.97 |
| 512 | 28.50 → 23.66 | 223.82 → 221.46 |
| 1024 | 34.09 → 28.68 | 1468.40 → 1465.76 |
| 2048 | 43.84 → 39.29 | 11575.50 → 11595.16 |

Encoding drops by approximately 5 µs. Whole-command GPU durations are noisy, particularly for large matrices, and do not isolate the blit stage. These measurements do not fully decompose the unprofiled wall-time gain: for example, profiled resident 128² wall medians are 305.97 → 303.35 µs, a smaller change than acceptance timing. Profiling results are retained rather than substituted for acceptance data.

All observed submission/compute-dispatch counts are unchanged: resident submissions=1, transfers=4, compute dispatches=3/4/4/4/5 for N=128/256/512/1024/2048. Budget peak at 2048² is 117,572,648 bytes in both versions (112.126 MiB). At 128² it falls from 459,300 to 459,296 bytes because the second scratch buffer would only have held the final scalar. Other tested peaks are unchanged. This metric is budgeted payload storage, not process RSS or driver memory.

## Final numerical and lifecycle qualification

All **43 selected CTest gates pass**: the expanded pipeline gate, hardware numerical optimization gate, benchmark-evidence unit gate and 40 software/map/matrix/graph/epilogue gates. The pipeline gate checks **237,877,413 output references** across optimized/debug/ASan+UBSan/TSan current and staged paths plus optimized old bundles, and 410,751,531 pipeline input-immutability words. Hardware qualification adds **23,220,880 output comparisons**, for **261,098,293 core output checks**, excluding additional references in the 40 other passing fixtures.

The hardware harness now checks 3,188 cases per mode, including every length 0…260, random/cancellation-sensitive/negative-zero patterns, 128-way multi-pass boundaries, complete timed-input matrix cells and small-case input immutability. Allocation injection, tight budgets, held cancellation/deadline/device failure, cleanup, scalar epilogues, resident graph reductions and old software/current shader entries pass. Removing allocations/encoders reduces exercised failure sites (20 current and 25 staged pipeline failures); tests dynamically exhaust every remaining preparation/execution checkpoint. Host sanitizers do not instrument GPU instructions. The two core GPU gates enable Metal API and shader validation; the 40 focused fixtures enable API validation.

Generated production pipeline `.metal` files, shader header, public runtime API and task scheduler match the grouped-traversal baseline byte-for-byte. Only the host reduction destination/copy handling changes. Source/artifact hashes and all final logs are captured by the qualification record. The offline audit validates 936 measured/profiled processes and 214 paired blocks, including the rejected alternative, A/A control and every retained losing block; calibration checks execute before timing but their raw samples are not persisted by the shared protocol.

## Evidence and reproduction

- [Identical-binary control](small-workloads-aa-2026-10-26.json).
- [Optional-pipeline loading ablation](small-workloads-loading-2026-10-26.json).
- [Rejected shuffle timing](small-workloads-shuffle-2026-10-26.json) and [source/provenance snapshots](small-workloads-experiments-2026-10-26.json).
- [Accepted host-only comparison](small-workloads-direct-2026-10-26.json).
- [Net comparison against pre-grouping production](small-workloads-net-2026-10-26.json).
- [Separate profiles](small-workloads-profile-2026-10-26.json), [evidence audit](small-workloads-evidence-audit-2026-10-26.json) and [final qualification](small-workloads-qualification-2026-10-26.json).

Use `native/benchmarks/small_workloads.py --help` for matched binary/bundle comparisons, or `--profile-only` for separate diagnostics. `native/benchmarks/small_workloads_audit.py` checks all recorded warmup/timed checksums, ABBA/BAAB order, equal per-pair iterations and recalculated paired ratios. The baseline archive hashes match the preceding qualification records; compiler/source/artifact hashes and exact commands accompany each record. Large generated artifacts stay under ignored `benchmarks/.build/`.
