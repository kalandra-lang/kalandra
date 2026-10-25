# Strict GPU input layout and weight packing — 2026-10-25

**Decision: retain the existing production kernel.** Packing produces useful square-case gains, including its GPU construction cost, but fails the broader shape tests. This change preserves reproducible experiments and every measured regression. It does not integrate packing, change language semantics or claim a new managed-pipeline speedup.

The study runs on Apple M2 Max/macOS 27 with the pinned Metal toolchain, targeting macOS 26. The preceding [stage profile](microkernel-investigation-2026-10-25.md) places 99.36% of sampled 2048² compute-stage ticks in matrix plus epilogue execution. That motivates changing global input layout while retaining the existing 16×16/64-leaf arithmetic kernel.

## Layouts and protocol

Eight alternatives accompany the unmodified kernel control:

| Name | Packed global input layout |
|---|---|
| `panel` | B: output-column tile → contraction block → contraction position → column |
| `block` | B: contraction block → output-column tile → contraction position → column |
| `column` | B transposed by column; padded threadgroup staging |
| `tpanel` | B: output-column tile → contraction block → column → contraction position |
| `swizzle` | B column XOR swizzle within each panel |
| `both` | Panel B plus A: output-row tile → contraction block → row → contraction position |
| `blockpad` | Block B plus a 128-byte gap after each contraction block's column tiles |
| `panelpad` | Panel B plus a 128-byte gap after each column tile's contraction blocks |

The first screen uses flat index/division construction; the second uses two-dimensional tiled construction, and the padding screen adds the last two layouts. GPU packing copies UInt32 words. Candidate matrix arithmetic keeps separately rounded products and the adjacent-pair tree, compiled with `-fno-fast-math -ffp-contract=off`. No vendor library, FMA substitution or altered accuracy policy is introduced.

Each screen has three randomized fresh processes at 512², 1024² and 2048², with two warmups and three measured rounds. R=1 constructs B for one use; R=8 constructs B once for eight calls with alternating activation inputs and eight distinct outputs. `both` additionally constructs A for every call. Every round constructs B anew. Later paired tests batch several complete pack-and-use iterations to target 20 ms GPU work (cap 512 iterations); packing is repeated in every iteration, so it is amortized only across the specified R calls. The seven-block square confirmation and five-block shape tests mirror ABBA/BAAB ordering and randomize shape order.

Inputs, output buffers and packed workspace are preallocated private buffers. GPU command timestamps include packing, matrix dispatches and command execution; host wall time includes encoding/commit/wait. Allocation, initial uploads, compilation/loading, downloads and correctness checks are outside timing. Thus these are numerical-kernel experiments, **not** full language/API throughput, cold-start latency, or end-to-end LLM inference measurements. No complete managed pipeline is timed for an unaccepted candidate.

## Square confirmation

Positive numbers are reductions in GPU command time, calculated per paired block as `1 − candidate/control`; negative numbers are regressions. Reported medians and full block ranges retain outliers rather than discarding them. These are observed ranges, not confidence intervals.

| Layout | Square size | Calls per B construction | Median reduction | Block range |
|---|---:|---:|---:|---:|
| `block` | 512 | 1 | +7.48% | +7.03% to +8.27% |
| `block` | 512 | 8 | +9.22% | +8.75% to +9.43% |
| `block` | 1024 | 1 | +9.34% | +6.96% to +9.96% |
| `block` | 1024 | 8 | +10.77% | +10.63% to +11.56% |
| `block` | 2048 | 1 | +10.63% | -5.37% to +16.25% |
| `block` | 2048 | 8 | +11.31% | +9.64% to +14.59% |
| `both` | 512 | 1 | +10.83% | +10.68% to +12.73% |
| `both` | 512 | 8 | +12.31% | +7.26% to +15.29% |
| `both` | 1024 | 1 | +6.10% | -26.05% to +11.17% |
| `both` | 1024 | 8 | +15.99% | +15.38% to +16.93% |
| `both` | 2048 | 1 | +15.17% | -9.01% to +15.57% |
| `both` | 2048 | 8 | +15.33% | +9.32% to +16.26% |

The square results alone would suggest integration: weights-only packing reduces the R=8 median by 9–11%; packing both inputs by 12–16%. But R=1 already has losing blocks at 1024²/2048², and larger shape coverage overturns a general adoption policy.

## Rectangular and contraction-boundary results

All shapes below use R=1 with fresh construction. Values are median paired GPU-time reductions. Each layout has its own contemporaneous control; compare the candidate against its control, not absolute timing between suites.

| M × K × N | Both inputs | B block | B block + gap | B panel + gap |
|---|---:|---:|---:|---:|
| 1024 × 512 × 1024 | +7.21% | +7.27% | +9.08% | +7.07% |
| 1024 × 1024 × 2048 | +3.73% | -4.95% | -13.53% | -11.36% |
| 1024 × 4096 × 1024 | -21.51% | +1.70% | -23.65% | -22.17% |
| 1024 × 4096 × 2048 | +6.80% | -1.10% | +6.64% | -27.75% |
| 1025 × 576 × 1027 | -13.81% | +8.89% | +9.07% | +8.33% |
| 2048 × 512 × 1024 | -14.39% | +8.49% | +8.83% | +9.73% |
| 2048 × 1024 × 1024 | -10.38% | +10.23% | +10.90% | +1.01% |
| 2048 × 4096 × 1024 | +2.82% | +17.53% | +17.24% | +17.99% |

Packing both inputs loses a median 21.51% at 1024×4096×1024 and 13.81% on the odd 1025×576×1027 case. B-only packing avoids several losses, but still regresses wide outputs. Adding gaps changes the outcome, including new 23–28% median losses. This is consistent with layout-sensitive memory behavior, but timestamps do not prove a cache-conflict, bandwidth or occupancy cause.

We then tested a less permissive B-only hypothesis: M,N≥1024, M≥N, K≤2M, within the existing register kernel's K=512…4096 and divisible-by-64 envelope. Eight additional interior/odd/deep shapes deliberately include sizes outside the original square suite. The policy is **rejected**, not installed in the runtime:

| M × K × N | Median reduction | Five-block range |
|---|---:|---:|
| 1024 × 2048 × 1024 | -4.47% | -22.62% to +10.57% |
| 1025 × 576 × 1025 | +8.89% | +8.31% to +9.79% |
| 1088 × 1024 × 1024 | +0.70% | -19.87% to +4.11% |
| 1536 × 768 × 1024 | +10.10% | +8.63% to +11.36% |
| 1536 × 1536 × 1536 | +0.11% | -15.07% to +5.79% |
| 1536 × 3072 × 1024 | +5.56% | -3.92% to +11.84% |
| 2048 × 512 × 2048 | +7.32% | +6.87% to +8.08% |
| 2048 × 4096 × 2048 | +6.22% | +0.45% to +10.77% |

The 1024×2048×1024 median regresses 4.47%, with a 22.62% losing block. At 1536³ the median is essentially unchanged, with a 15.07% losing block. The 1088-row case also has a roughly 20% losing block. Some other interior shapes gain consistently, but this is insufficient evidence for a general policy. Restricting dispatch to the three original benchmark square sizes would hide this failure.

The retained records contain 1,442 measured processes, 104 calibration processes and 284 paired blocks. The offline audit verifies source snapshots, complete round coverage and paired calculations against the underlying samples.

## Memory, ownership and correctness boundaries

B-only packing requires `4 × ceil(N/16) × 16 × K` scratch bytes; packing both adds `4 × ceil(M/16) × 16 × K`. Padding adds 128 bytes per K-block or N-tile. Guard regions add 64 bytes per experimental scratch allocation. At 2048² the extra scratch is 16 MiB for B, or 32 MiB for both, plus guards. The R=8 driver owns 176 MiB private baseline buffers, 192 MiB with B packing, or 208 MiB with both (rounded, including eight distinct outputs and two activation inputs). Shared upload/readback buffers add about 64 MiB in each case. These are explicit allocated buffer lengths, **not RSS**, and do not represent the managed runtime's peak accounting. Reuse does not eliminate the packed copy's storage cost.

The diagnostic never modifies original inputs and never retains packed data across command rounds. It checks every output cell in all R outputs against an independent integer reference, packed words and inactive/gap zero padding, poisoned suffix guards and original input immutability after every warmup/measured round. Batched rounds inspect the final identical iteration's outputs. Dyadic inputs make the strict tree exact for these shapes; this is screening, **not** a full arbitrary-float/APFloat/NaN/subnormal or cancellation/ownership qualification of a new kernel. Host sanitizers do not instrument GPU instructions.

A managed packing prototype was explored locally, including budget fallback and acknowledgement-held scratch tests, then removed after the shape-policy failures. It is not part of this commit and its exploratory passes are not counted as qualification of a shipped implementation. Compiler, shaders, runtime and selection policy are restored to the preceding production source exactly. Default 64 MiB GPU budget, cancellation, allocation failures, acknowledgement ownership and numerical behavior are unchanged. No immutable-weight cache or new public API is shipped.

## Evidence and reproduction

Raw sources, generated MSL, per-round samples, commands and hashes are retained in these records:

- [screening](weight-packing-screening-2026-10-25.json)
- [tiled-screening](weight-packing-tiled-screening-2026-10-25.json)
- [confirmation](weight-packing-confirmation-2026-10-25.json)
- [boundaries](weight-packing-boundaries-2026-10-25.json)
- [right-boundaries](weight-packing-right-boundaries-2026-10-25.json)
- [padding-screening](weight-packing-padding-screening-2026-10-25.json)
- [padded-boundaries](weight-packing-padded-boundaries-2026-10-25.json)
- [panel-boundaries](weight-packing-panel-boundaries-2026-10-25.json)
- [envelope](weight-packing-envelope-2026-10-25.json)

Run from the repository root on the qualified Mac with Metal installed:

```sh
python3 native/benchmarks/weight_packing_audit.py
python3 native/benchmarks/weight_packing.py --pack-method tiled --output /tmp/packing-screen.json
python3 native/benchmarks/weight_packing_confirm.py --screen /tmp/packing-screen.json --output /tmp/packing-confirm.json
python3 native/benchmarks/weight_packing_boundaries.py --screen /tmp/packing-screen.json --layout block --suite boundaries --output /tmp/packing-boundaries.json
python3 native/benchmarks/weight_packing_boundaries.py --screen /tmp/packing-screen.json --layout block --suite envelope --output /tmp/packing-envelope.json
python3 native/benchmarks/weight_packing.py --pack-method tiled --layouts baseline blockpad panelpad --output /tmp/packing-padding.json
```

Use a fresh output path; tools refuse to overwrite evidence. The confirmation/boundary runners verify the screen library hash. Historical raw records embed the version of the generator/driver used then. Artifact directories are disposable and ignored; records remain tracked. Screen runs require access to the physical GPU and should run without competing GPU tests, shader validation or build workloads. Validation is a separate run.

All **44 focused restored-production regression gates pass**, including debug, optimized, ASan/UBSan and TSan pipeline modes with 58,726,376 output-cell checks, plus matrix/graph/indirect/ReLU and codegen/oracle gates. All nine diagnostic layouts also pass [Metal API/shader validation](weight-packing-shader-validation-2026-10-25.json) on 17×64×19 and 1025×576×1027, with R=2 and every warmup/output checked. Validation is untimed and does not qualify a candidate runtime.

[Restored-production qualification and evidence integrity](weight-packing-qualification-2026-10-25.json) records final source/artifact hashes and checks. Rebuilt binaries are hashed independently; no byte-identity claim is made for rebuilt archives. The preceding qualified performance remains the [epilogue pipeline](epilogue-pipeline-2026-10-25.md). The next useful experiment needs to explain the layout-sensitive losses—for example, a targeted Metal capture and controlled access-stride study—before another dispatch policy is justified.
