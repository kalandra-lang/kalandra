# Hardware GPU compute optimization

This qualification targets hardware float32 on the Apple M2 Max. It compares the same source kernels against the optimized, single-threaded Kalandra CPU implementation. Explicit software arithmetic and unsupported Metal float64 retain their existing numerical implementations.

## Measured bottlenecks

The original composed map/matrix-plus-sum path submitted two command buffers and crossed the external-task completion boundary between them. Its balanced reduction dispatched once per tree level: a 4,194,304-element dot product required 23 dispatches, including its map. GPU command timestamps and host encoding counters separate device execution from encoding and wall time. These counters are opt-in (`metal::Options::profile`); performance acceptance runs disable them, API validation and shader validation.

## Compiler and runtime changes

Hardware float32 bundles include an independently compiled threadgroup reduction entry. Sixty-four lanes reduce 128 contiguous leaves using the same adjacent-pair grouping, carrying missing odd leaves without adding padding zeros. Each global pass reduces the length by 128 instead of two. All inputs remain immutable, count words remain immutable through acknowledgement, and alternating scratch buffers retain their tracked dependencies.

Pure hardware maps and matrix products immediately followed by a sum encode both stages into one command buffer. The invocation retains the original inputs, mapping parameters, kernels and intermediate storage until completion. Cancellation still prevents publication, and allocation/command failures unwind the same invocation ledger. Checked numeric/control paths retain their stage boundaries; resident float32 graphs use the separately qualified batching and fusion described below.

The hardware matrix entry uses 8×8 shared-memory tiles for operand reuse. Each contraction block computes the adjacent-pair product tree; a binary-counter tree combines full blocks and carries the partial tail in the original order. The previously qualified register-quad path remains selected in its existing M2 Max crossover range. Products and sums remain separate operations, compiled with `-fno-fast-math -ffp-contract=off`.

## Numerical qualification

`native-metal-gpu-optimization` uses an independent LLVM APFloat oracle with Metal hardware float32 flush-to-zero applied at arithmetic boundaries. It compares all non-NaN results bitwise, including signed zeros; NaN payload equality is not required. The suite includes seeded mantissas and exponents, cancellation and rounding cases, infinities/NaNs/subnormals, empty axes, tile tails and contraction/reduction boundaries. Both direct matrix outputs and composed scalar sums are checked.

Every output cell of the actual benchmark-input matrices at 128, 256, 512 and 1024 is additionally checked against exact integer contractions, using their independent periodic input phases. Allocation injection, held-completion cancellation/deadlines, failed command completion, single-submit composition and complete cleanup are checked. These tests run against optimized, debug, address/undefined-sanitized and thread-sanitized runtimes with Metal API and shader validation enabled.

## Reproduction

Run `python3 native/benchmarks/languages/run.py --suite gpu-scaling --languages kalandra kalandra_gpu_resident kalandra_gpu_transfers --output <new.json>` for uninstrumented CPU/resident/transfer comparisons. Add `--profile` for command timestamps and encoding counters, or `--quick` for explicitly exploratory measurements. Inputs span 262,144–4,194,304 vector elements and 128–1024 square matrices. The existing default workload suite and sampling protocol remain available unchanged. Every measured and warmup batch is checked against an independent exact integer checksum. Matrix checksums use integer distributivity to evaluate the reference in quadratic time; runner unit tests compare it with the cubic definition.

## Recorded qualification

The [GPU speedup qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/gpu-speedup-2026-10-24.md) records the final full runs: roughly 20× CPU speedup for larger resident matrices, 13–18× including uploads, and 1.29–1.31× for resident vectors at 16,777,216 elements. Smaller vectors and fresh uploads remain CPU-favored. Both acceptance reports use the same compiler/backend fingerprints and uninstrumented three-process sampling.

All four runtime variants passed 1,010 numerical cases and 1,427,215 output comparisons each with Metal validation enabled; an optimized run without validation also passed. All 46 selected GPU/runtime tests passed across the broad run and targeted reruns, including the corrected higher-rank admission controls, legacy software arithmetic, numeric storage and graph paths. The complete native build and eight benchmark protocol/reference unit tests passed.

## Follow-up after the fair comparison

Hardware float32 matrix bundles also include a 32-leaf contraction-block entry with an 8×8 output tile. Each group stages 256 elements of each operand, reduces four exact eight-leaf subtrees into one 32-leaf root, then combines roots with the existing ordered carry stack. Two barriers cover 32 contraction elements instead of eight. Products and sums remain separately rounded; no matrix intrinsic, FMA, reassociation or padded arithmetic is substituted.

The new entry is selected for contraction lengths at least 256 and divisible by 32 where the generic hardware tile would otherwise run. Existing quad-profile selection is retained; non-multiple contractions keep the admitted eight-leaf implementation. Per-cell APFloat qualification includes 256/288/320/544 contractions, row/column edges, non-power-of-two root counts, exceptional values and the actual benchmark matrices through 1024². All debug/optimized/sanitizer variants remain covered.

The [fair optimization follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/optimization-2026-10-24.md) compares complete CPU/resident/upload-inclusive tasks with optimized library APIs. It preserves the earlier benchmark-methodology correction and makes no general language ranking from GPU tensor performance.

## Larger output tiles

The [matrix tuning follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/matrix-tuning-2026-10-25.md) adds an optional 16×8 output tile with 128 lanes. It stages 1,024 left and 512 right float32 values, reusing each RHS panel across sixteen rows. Two barriers cover each exact 64-leaf contraction subtree. All lanes reach both barriers; out-of-range output lanes do not perform arithmetic. The ordered carry stack preserves non-power-of-two root counts.

This entry requires at least sixteen rows, eight columns, and a contraction length of at least 512 divisible by 64. The existing quad profile takes precedence. Pipelines supporting fewer than 128 threads, older bundles, smaller shapes, and other contraction tails retain the previous admitted paths. Dispatch sizing now follows the selected layout; the quad entry no longer launches the larger 8×8 hardware tile's work-item count. Allocation, cancellation, submitted-buffer ownership, scalar readback and arithmetic policy retain their contracts.

Qualification includes 511/512/513, 575/576/577 and 640 contractions, 15/16/17 and 31/32/33 row boundaries, APFloat exceptional/rounding checks, and every cell of the timed square matrices through 2048² in all four runtime build modes.


## Resident graph map fusion

Pure float32 matrix graphs can lift a following same-cell unary map (including ReLU and captured scalar arithmetic) into the matrix kernel's final store. The compiler emits optional fused entries only for a unary read of the immediately preceding matrix. The runtime additionally checks single consumption, matching supported hardware pipelines, shape, scalar capture layout, workspace headroom and the existing batching allowance. Shared matrix values, indirect/unsupported maps, checked numeric programs, interleaved graph reductions, empty outputs and older bundles retain their staged behavior.

Fusion preserves separately rounded matrix products, the exact adjacent-pair contraction tree and the post-map's operation order. It removes a matrix-sized intermediate and one dispatch per eligible edge. The four **logical** stage allowance still counts both operations; fusion does not extend cancellation's batching allowance. Both parameter blocks and all borrowed inputs remain alive and charged through actual GPU acknowledgement. Tight budgets can select the staged path. The final graph sum remains separately submitted.

The [graph fusion study](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/graph-fusion-2026-10-26.md) reports complete resident timings, mixed/regressing cases, dispatch counts and payload peaks. Shader preparation/loading is outside those timings; optional entries add preparation work. This is a numerical runtime optimization, not evidence of general C++/Rust compiler parity.
