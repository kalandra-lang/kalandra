# Strict one/two-row GPU projections — 2026-10-26

The optional hardware-float32 profile parallelizes the contraction for one- and two-row matrices instead of assigning an eight-row tile to a single-row product. Two complete managed resident-pipeline comparisons reduce median elapsed time by **17–70%** and **14–69%** on the eligible tested shapes. Every eligible resident paired block improves in both runs. These are comparisons with the preceding Kalandra implementation, not Rust/C++ rankings, optimized-library parity or full-model LLM/token-throughput claims.

Physical qualification is Apple M2 Max/macOS 27.2; generated Metal targets macOS 26. Production enables the optional entries only in the qualified M2 Max profile. All other devices/types/shapes retain earlier entries. No vendor library, input packing, persistent weight cache, extra per-call buffer, relaxed arithmetic or cancellation opt-out is introduced.

## Kernel and dispatch boundary

The original small-row hardware path uses an 8×8 output tile: at one row only eight of its 64 lanes produce output, while all lanes help load shared panels and synchronize. The selected candidate uses a 32-column tile with 256 lanes: eight workers compute adjacent 32-product subtrees per output, then join their roots in the original adjacent-pair order. Panels span 256 contraction leaves, with two workgroup barriers per panel. Workgroups for adjacent rows of the same column tile are neighbors in logical ID, allowing an experiment in weight reuse without changing the row-major input representation. Actual physical scheduling/cache reuse is not guaranteed by logical order.

The source keeps individually rounded products, the exact ordered addition tree, and odd tails carried without padded-zero additions. A bounded private root stack combines complete panels; partial panels reduce only present roots. K≤16384 bounds the roots below nine slots. Each output has one producer; partial columns participate in barriers but never read/write out of range. Static threadgroup storage is 1 KiB. Bias/scale/ReLU lifting uses the same output index and canonical helper as existing matrix entries.

The host selects plain/fused optional entries only for **M=1 or 2, 512≤K≤16384 with K divisible by 32, and 256≤N≤16384**, after validating pipeline support for 256 threads. These are performance/implementation bounds, not language shape limits. Four/eight-row replacements lose on important cases and are rejected; they keep the old kernels. Kernel loading adds two optional cold pipeline objects (plain/fused), excluded from throughput timing. Private metadata/driver storage is outside budgeted tensor-byte measurements.

## Screening and independent holdouts

Six supported row-major configurations vary columns, contraction workers and 32/64-leaf subtrees. A seventh, 512-thread configuration is rejected before timing because the M2 Max pipeline supports only 384 threads. The initial unpaired screen has two randomized processes and is exploratory, not acceptance evidence. Column-major workgroup order is then tested in two six-block mirrored ABBA/BAAB comparisons, including losing four/eight-row cases. The selected 32-column/eight-worker/32-leaf candidate has:

| M × K × N | Paired preallocated GPU time reduction (full range) |
|---|---:|
| 1 × 576 × 1027 | +66.71% (+49.59…+71.64%) |
| 1 × 2048 × 2048 | +52.64% (+33.73…+68.13%) |
| 1 × 4096 × 4096 | +44.34% (+41.06…+46.58%) |
| 1 × 8192 × 2048 | +69.71% (+67.11…+70.88%) |
| 2 × 4096 × 4096 | +15.87% (+5.77…+24.83%) |
| 3 × 1984 × 2049 | +40.20% (+32.42…+43.37%) |
| 4 × 4096 × 4096 | -20.79% (-22.05…-17.86%) |
| 8 × 4032 × 1025 | +41.04% (+30.09…+43.10%) |
| 8 × 4096 × 4096 | -51.66% (-57.85…-44.33%) |

A separate six-block holdout checks independent widths, long contractions, non-power-of-two tails and partial columns. All 90 holdout blocks improve:

| M × K × N | Paired preallocated GPU time reduction (full range) |
|---|---:|
| 1 × 512 × 256 | +52.17% (+40.36…+69.87%) |
| 1 × 512 × 16384 | +45.16% (+42.83…+48.08%) |
| 1 × 544 × 257 | +49.70% (+25.84…+63.27%) |
| 1 × 1984 × 1536 | +60.96% (+38.01…+62.12%) |
| 1 × 4096 × 8192 | +56.60% (+51.48…+59.03%) |
| 1 × 8192 × 4096 | +45.05% (+37.43…+50.77%) |
| 1 × 11008 × 1024 | +74.66% (+72.28…+74.78%) |
| 1 × 16384 × 4097 | +44.81% (+43.37…+45.53%) |
| 2 × 512 × 256 | +54.37% (+24.88…+69.95%) |
| 2 × 576 × 1027 | +67.50% (+65.80…+69.31%) |
| 2 × 2048 × 8192 | +20.49% (+15.03…+25.23%) |
| 2 × 4096 × 2049 | +57.34% (+52.37…+57.69%) |
| 2 × 8192 × 2048 | +53.72% (+51.90…+53.91%) |
| 2 × 16352 × 1025 | +77.26% (+74.13…+78.44%) |
| 2 × 16384 × 1024 | +77.90% (+76.31…+79.29%) |

Diagnostics preallocate private inputs/outputs, share one immutable weight matrix, alternate two distinct activation/output pairs, and check both complete outputs, suffix guards and input bytes after every warmup/round. Every timed batch includes both calls; the reported GPU duration divides by twice the iteration count. Initial upload/allocation/loading and checking are excluded. An independent integer reference validates exact dyadic products. This diagnostic screening is separate from full arbitrary-float/runtime qualification and managed throughput.

The first holdout attempt retained an obsolete K%64 diagnostic input restriction; it rejected K=544 before any numerical comparison. The driver was corrected to admit valid 32-leaf contractions and the full holdout rerun passed. That fixture failure and the hardware-resource rejection remain recorded. Holdout driver source metadata was corrected after a formatting-only edit during the run: the exact compiled source, original record/hash and layout-header snapshot are retained, and clang-format reproduces the originally reported formatted snapshot. No samples/artifact hashes were changed. Future diagnostic runs assert that source inputs remain frozen during timing.

## Complete managed pipeline

Baseline is qualified commit `650ebf78921751a977245daf2364b7a0aeb7b075`. Matched drivers link the same task archive and use the same current bundle; only the Metal runtime archive differs. The old runtime ignores optional skinny entries. The old shader prefixes are byte-identical. Both versions execute the same dynamic rectangular source algorithm: `matmul → bias → scale → ReLU → balanced sum`.

The first run has six mirrored ABBA/BAAB blocks; the resident confirmation has eight. Each fresh process has two full warmup batches and three measured rounds, with equal per-pair iteration counts calibrated toward 20 ms. Every warmup/calibration/timed scalar must match the independent fixed-point, separately rounded tree reference. A new rectangular-reference unit test independently recomputes cubic products and all adjacent additions, including long/ragged contractions. The timer includes fresh intermediate allocation, scheduling, dispatch, scalar completion/readback, and complete cleanup; compilation/loading are excluded. No affinity, frequency or desktop isolation is imposed.

Positive percentages mean lower elapsed time. Percentages are medians of per-block paired ratios; full observed ranges are not confidence intervals. Microseconds are medians of block means and need not reproduce the median ratio.

| M × K × N | Run 1 old → new, µs | Run 1 paired reduction (range) | Resident confirmation (range) |
|---|---:|---:|---:|
| 1 × 544 × 257 | 265.93 → 225.60 | +16.91% (+12.84…+22.01%) | +17.57% (+12.81…+23.35%) |
| 1 × 2048 × 2048 | 400.09 → 263.04 | +31.99% (+26.89…+35.88%) | +34.00% (+32.69…+40.20%) |
| 1 × 4096 × 4096 | 628.43 → 425.17 | +32.36% (+30.17…+34.40%) | +32.24% (+21.21…+34.64%) |
| 1 × 8192 × 2048 | 921.23 → 452.81 | +50.72% (+46.38…+57.14%) | +51.87% (+48.66…+55.94%) |
| 1 × 11008 × 1024 | 1138.47 → 468.51 | +58.85% (+53.42…+62.44%) | +59.92% (+56.54…+64.39%) |
| 2 × 576 × 1027 | 275.83 → 226.99 | +19.85% (+10.99…+22.36%) | +14.43% (+11.93…+19.47%) |
| 2 × 4096 × 4096 | 649.10 → 533.69 | +17.97% (+15.80…+20.06%) | +19.81% (+14.22…+25.78%) |
| 2 × 16384 × 1024 | 1655.30 → 503.84 | +69.52% (+67.74…+69.89%) | +69.09% (+66.30…+70.72%) |

All eligible resident blocks improve in both runs. The largest tested managed reduction is about 70%, corresponding to roughly 3.3× the previous throughput for that component. This is a scalar-consumer projection benchmark; it does not measure attention, KV caches, quantized weights or a complete LLM decoder.

The extended driver preserves its square CLI and adds rectangular dimensions plus three transfer modes. The benchmark budget is explicitly `max(64 MiB,16*(M*K+K*N+M*N)+4096)`; the square case is unchanged from earlier pipeline runs. Larger weight tests require this raised benchmark budget. A separate physical run of 1×2048×2048 with the unmodified 64 MiB default passed, with a 50,356,224-byte payload peak; production defaults do not change.

## Transfers and unchanged controls

`resident` uploads A/B/bias before timing. `activations` retains B and uploads A/bias per call. `transfers` uploads all three per call. Both upload modes still include the same scalar consumer/cleanup:

| M × K × N | Resident weights + activation/bias uploads | All three uploads |
|---|---:|---:|
| 1 × 544 × 257 | +9.30% (+1.96…+15.48%) | +6.46% (+1.54…+13.90%) |
| 1 × 2048 × 2048 | +17.64% (+16.77…+25.73%) | +4.65% (+1.89…+6.74%) |
| 1 × 4096 × 4096 | +19.78% (+15.48…+22.39%) | +3.28% (-1.51…+7.42%) |
| 1 × 8192 × 2048 | +38.61% (+32.01…+41.00%) | +8.09% (+6.42…+10.58%) |
| 1 × 11008 × 1024 | +45.75% (+43.74…+46.79%) | +7.89% (+5.69…+14.00%) |
| 2 × 576 × 1027 | +9.70% (-21.73…+14.00%) | +5.50% (-4.08…+10.61%) |
| 2 × 4096 × 4096 | +12.76% (-16.39…+16.70%) | -0.25% (-75.58…+5.27%) |
| 2 × 16384 × 1024 | +55.29% (+54.62…+58.40%) | +9.63% (+6.38…+12.52%) |

Uploads dilute or obscure the arithmetic gain. The full-upload 2×4096×4096 median is −0.25%, with a retained −75.58% losing block; there is no blanket transfer-throughput win. Activation-upload medians improve on all eligible shapes but retain losing blocks.

The following resident controls do not select the new entry:

| M × K × N | Run 1 paired reduction (range) | Confirmation (range) |
|---|---:|---:|
| 4 × 4096 × 4096 | +0.63% (-1.94…+3.49%) | -0.69% (-2.48…+3.66%) |
| 8 × 4096 × 4096 | +0.84% (+0.14…+1.28%) | +0.20% (-2.78…+1.18%) |
| 128 × 128 × 128 | -1.11% (-4.15…+5.54%) | -1.47% (-3.85…+5.85%) |
| 256 × 256 × 256 | +0.27% (-3.21…+3.70%) | +0.20% (-1.76…+3.27%) |
| 512 × 512 × 512 | -0.21% (-8.04…+3.83%) | -1.11% (-4.34…+1.99%) |
| 2048 × 2048 × 2048 | -0.59% (-1.88…-0.17%) | +0.23% (-2.71…+1.29%) |

Small controls are **not claimed regression-free**: 128² loses 1.11–1.47% by median in both runs; 512² loses 0.21–1.11%. The 2048² result moves from −0.59% to +0.23%. An eight-block identical-binary/bundle A/A control records −0.26% at 128², −0.57% at 512² and −0.25% at 2048², with small-shape block ranges spanning approximately −3.5…+3.7%. Variation overlaps the small losses but does not prove they are all noise. They remain disclosed and their precise cause is unresolved.

## Separate profiling

Four alternating-order blocks per version collect short, separately profiled resident batches. These medians are microseconds per call, **not acceptance timings**:

| M × K × N | GPU command, old → new | Host encoding, old → new |
|---|---:|---:|
| 1 × 2048 × 2048 | 183.59 → 59.08 | 20.39 → 18.28 |
| 1 × 4096 × 4096 | 416.23 → 230.71 | 26.09 → 24.46 |
| 1 × 8192 × 2048 | 787.09 → 211.46 | 22.77 → 20.56 |
| 2 × 16384 × 1024 | 1437.10 → 402.96 | 18.88 → 18.93 |
| 128 × 128 × 128 | 109.06 → 133.61 | 20.86 → 21.21 |
| 512 × 512 × 512 | 362.64 → 638.93 | 22.65 → 21.88 |
| 2048 × 2048 × 2048 | 11598.31 → 11570.03 | 33.83 → 36.21 |

The intended shapes show lower GPU duration. Small-control GPU times vary widely; encoding measurements do not explain the repeat small losses. No cache, occupancy or branch-cost mechanism is claimed from these counters. Profiles are not substituted for the unprofiled paired results.

Submission/compute-dispatch counts remain unchanged: eligible resident projections use one submission and three compute dispatches; the activation-upload mode adds two submissions, and full uploads add three. Profiled payload peaks match between versions on every resident shape: 50,356,224 bytes for 1×2048×2048, 201,375,744 for 1×4096×4096, 201,596,928 for 2×16384×1024 and 117,572,648 for 2048². These are process-lifetime budget peaks, including initial uploads, not isolated per-call peaks, RSS or driver pipeline storage. The added kernel introduces no host/device tensor-buffer allocation site.

## Safety and qualification

All **45 selected CTest gates pass**: canonical epilogue lifting, seven benchmark-evidence unit tests, the expanded pipeline/hardware suites, 40 map/matrix/graph/indirect gates and all-ten-type ReLU. The two core GPU suites enable API/shader validation. Optimized/debug/ASan+UBSan/TSan paths check **270,631,255 core output references**, excluding additional references in the 41 broad fixtures. Pipeline fixtures check **606,779,480 input words** for immutability; register tests separately verify both original inputs. Host sanitizers do not instrument GPU/driver instructions.

Plain matrix qualification expands to 147 cases per bundle/mode, covering non-dyadic values, NaNs/signaling inputs, signed zero, subnormals, cancellation-sensitive sums, K/N limits and every rejected boundary. Fused/staged qualification expands to 55 cases per bundle/mode. Existing bundles strip both optional grouped/skinny entries; a mixed bundle pairs a current matrix with an older post-map and verifies that its dispatch geometry follows the selected fused entry. Current, staged, old and mixed paths pass complete-cell references.

Both plain and fused eligible skinny paths exhaust preparation/execution allocation faults, held cancellation/deadline/device failures, immutable capture checks and cleanup. Existing tight-budget and graph/indirect checks remain passing. No new per-call allocation is introduced; existing acknowledgement-held resource ownership and budget reservations remain in force. The public runtime header, task scheduler and original hardware-shader header are unchanged. Production kernel tokens match the selected diagnostic candidate after entry renaming/whitespace normalization.

## Evidence and reproduction

- [Exploratory screen](skinny-screening-2026-10-26.json), [resource rejection](skinny-resource-rejection-2026-10-26.json), [paired order/layout confirmation](skinny-order-confirmation-2026-10-26.json), [independent holdouts](skinny-holdout-2026-10-26.json).
- [Complete three-mode pipeline](skinny-pipeline-2026-10-26.json), [resident confirmation](skinny-pipeline-confirmation-2026-10-26.json), [identical-binary control](skinny-control-aa-2026-10-26.json).
- [Default-budget validation](skinny-default-budget-2026-10-26.json), [separate profiles](skinny-profile-2026-10-26.json), [evidence audit](skinny-evidence-audit-2026-10-26.json), [qualification and hashes](skinny-qualification-2026-10-26.json).

Use `native/benchmarks/skinny_matmul.py --help` for preallocated screens/confirmation/holdouts and `native/benchmarks/skinny_pipeline.py --help` for managed runs. Build once with `--build-only`; subsequent `--reuse-build` runs verify frozen source/artifact hashes. `--profile-only` is diagnostic and excluded from acceptance timing. `native/benchmarks/skinny_audit.py` recalculates every paired ratio, checks recorded warmups/timed checksums and source snapshots, and checks complete output counts in every diagnostic round. Managed calibration samples are checked during execution but not persisted by the shared protocol. Large generated artifacts remain under ignored `benchmarks/.build/`.
