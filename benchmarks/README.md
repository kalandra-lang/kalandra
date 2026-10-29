# Kalandra native benchmarks

Latest GPU storage study: [invocation-private parameter arena](results/gpu-parameter-arena-2026-10-29.md), with [chart](results/gpu-parameter-arena-2026-10-29.png). It isolates host/driver costs, rejects three completion-worker alternatives, and qualifies shared argument backing with unchanged command boundaries. Primary resident medians improve 3.0–9.6%; the small all-upload regression, controls and replication are retained.

Latest GPU scheduling study: [work-bounded resident progression](results/gpu-work-bounded-progression-2026-10-29.md), with [chart](results/gpu-work-bounded-progression-2026-10-29.png), compiler cost proofs, cancellation/lifetime qualification, same-binary control, replication, fresh CPU/MPSGraph comparisons and retained contended diagnostics. All 27 case medians improve; resident time falls 10.5–55.8%. The largest case is 7.10× our CPU speed but still takes 2.93× MPSGraph time.

Latest AI study: [verified CPU/GPU optimization sequence and complete transformer block](results/native-ai-performance-2026-10-28.md), with the [comparison chart](results/native-ai-performance-2026-10-28.png). It includes paired baselines, CPU numerical libraries, benchmark-only MPSGraph, per-cell correctness and the remaining typed GPU lifecycle gap.

Earlier numerical study: [residual feed-forward CPU/GPU and optimized libraries](results/residual-ffn-2026-10-28.md). Two complete runs compare a bias-free expanding/contracting ReLU block with a residual connection, including resident, activation-upload and all-upload Metal modes. Each run checks 2,159,464 output cells. The study identifies CPU matrix execution and GPU residency/submission costs as follow-up areas; it does not establish full LLM or vendor-GPU-library parity. The [updated CPU comparison chart](results/cpu-comparison-2026-10-28.png) separately visualizes the committed wide/signed source-algorithm results ([editable SVG](results/cpu-comparison-2026-10-28.svg)).

Latest follow-up: [wide and signed CPU inputs](results/wide-input-final-2026-10-28.md). A new 54-case corpus crosses signed, 16-bit and 32-bit data boundaries with cyclic runtime inputs and up to 4096 rounds. Both final runs qualify all 54; the six-process aggregate beats both references in 29, with a worst gap of 6.6%. Fixing unsigned-only reduction replay cuts negative-array time 50–59%; count-only floating unrolling and signed one-round proofs improve short scalar work. Existing small-input and legacy controls still qualify. 432 selected CPU checks passed after one unchanged serial rerun of a documented observer-thread race; 50 Metal checks and Windows/Linux/macOS qualification, including notarization, passed.

Earlier follow-up: [stable integer recurrences and constant-round buffers](results/steady-loop-final-2026-10-28.md). The expanded 36-case corpus qualifies in both complete runs; aggregate process medians put 31/36 cases ahead of both references, with a worst gap of 4.8%. Paired one-round arrays improve 94.0–98.6%; a changing-state GCD control improves 12.9%/6.3% at 16/256 rounds. Repeated-GCD convergence gains are reported separately. Exact one-round proofs retain small-workload performance; 432 selected CPU and 50 selected Metal checks passed, as did Windows/Linux packages and macOS signing/notarization.

Use the [native benchmark guide](../native/benchmarks/README.md) for CPU kernels, scheduler overhead, tensor shapes and Apple Metal measurements. The harnesses live in `native/benchmarks`; raw samples, commands, source hashes and reports live in `benchmarks/results`.

Earlier follow-up: [bounded private-buffer reduction folding](results/wide-reduction-final-2026-10-28.md). Paired zero-round time falls 79%, 89% and 96% for widths 32, 64 and 128. A 32-element one-round case remains 2.5% slower than baseline. Three full runs meet the 10% Rust/C++ target in 32/32, 31/32 and 32/32 cases. The nine-process aggregate qualifies all 32 and beats both references in 27; the noisy GCD repeat miss is retained. Beating both everywhere remains open. The prior [direct-buffer and Windows qualification](results/buffer-direct-final-2026-10-27.md) and [scalar entry work](results/scalar-entry-final-2026-10-27.md) retain their full evidence.

Earlier follow-up: [guarded private-array reductions](results/reduction-ranges-2026-10-27.md). Adaptive warm-batch runs meet the 10% target in 29/32 cases, including all 20 array cases. Paired zero-round sums improve about 3.0–3.7× and one-round maps about 1.6–2.0× for widths 32–128. Three one-round scalar cases remain open. Whole-region bounds, exact range witnesses and checked musttail replay preserve failure behavior; the original eight-case control still qualifies. Earlier short-batch data is retained separately.

Earlier follow-up: [broader ordinary CPU corpus](results/broad-ordinary-2026-10-27.md). Two strict floating recurrences, Euclidean GCD and arrays up to 128 elements expand the comparison to 32 cases. Both final runs meet the 10% target in 24/32 cases; all 16/256-iteration cases qualify, while eight short cases still miss. Paired floating 16-iteration time falls 23.5–25.2%; new wide-array time falls 26.2–84.9%. Guarded countdown/IEEE proofs and four size-adjusted private buffers retain checked failure behavior. The original eight-case control still qualifies. The larger corpus does **not** meet the target everywhere.

Earlier follow-up: [ordinary entry ABI and private buffer lifetime](results/ordinary-abi-2026-10-27.md). All eight ordinary runtime-input cases are within 10% of the faster C++/Rust reference in both complete runs. Arrays are roughly 3.2–3.8× faster in the first run. Paired scalar zero-round time falls about 46%; the short scalar case is about 1.4% slower than the previous compiler while retaining its lead over the references. Checked arithmetic, first-fault locations and task cancellation remain intact. These results qualify the recorded cases on this host, not the whole language or all platforms.

Earlier follow-up: [checked entry frames and rejected backing-layout experiment](results/entry-costs-2026-10-27.md). Isolated paired zero-round scalar entry takes 10.0% less time; the short 16-round scalar case is 1.7% slower. Other nonempty paired source medians stay within 0.6% of baseline. The compact backing prototype regressed arrays and was removed. Zero-round Rust/array targets remain open; nonempty fair cases remain within 7.2% of C++/Rust across the final runs.

Earlier invocation results: [contract-aware native entry](results/unbound-entry-2026-10-26.md). The compiler omits managed entry for proven ordinary code while retaining cancellable execution. Nine same-contract paired blocks show 68.5% less zero-round scalar time; short-loop arithmetic specialization is retained and nonempty paired medians remain within 0.6% of baseline. Nonempty fair cases stay within 7.6% of C++/Rust across ordinary/repeat/cancellable runs; production recursion stays within 5.8%. Zero-round Rust-scalar and array targets remain open. Earlier [lifecycle/ABI qualification](results/invocation-lifecycle-2026-10-26.md) and [range/checkpoint work](results/range-foundation-2026-10-26.md) retain their historical measurements, misses and controls.

Earlier source-call implementation and qualification: [native call foundation](results/call-foundation-2026-10-25.md), including native internal returns, removal of the 128-call cap, physical stack guards, ARM64 remainder arithmetic, exact numerical checks and full before/after timing tables. Earlier follow-ups below retain their historical measurements and completion status.

Measurements describe their recorded hardware, toolchain and workloads. Read each report’s numerical, ownership and timing boundaries before comparing results. Generated benchmark artifacts belong in the ignored `benchmarks/.build` directory.

`benchmarks/.build` is disposable: it contains binaries, compiler/module caches and temporary outputs, and the harnesses recreate it. Keep `benchmarks/results` with the reports they support. Its raw JSON contains samples and artifact hashes used by the qualification evidence; deleting those files would leave unsupported reports and broken references. The raw text evidence compresses well. If storage becomes a concern, move them to a versioned compressed evidence archive and update the readers and references together.

For equivalent source algorithms against Go, Python, Rust and C++, plus optimized OpenBLAS/NumPy/Accelerate numerical APIs, use the [fair comparison suite](../native/benchmarks/fair/README.md) and [measured results](results/fair-benchmarks-2026-10-24.md). Numerical cases include Kalandra CPU and Apple Metal resident/upload-inclusive modes. General source recursion has no supported GPU execution path.

The earlier [tensor implementation suite](../native/benchmarks/languages/README.md) remains a historical comparison of different implementation strategies; it does not qualify compiler parity.

Latest CPU optimization results: [built-in tensor versus handwritten implementation comparison](results/cpu-parity-2026-10-24.md). These results do not establish the requested Rust/C++ compiler-performance parity; see the [methodology correction](../docs/design/benchmark-methodology.md). Full profiles include both GPU modes: [optimized float32](results/languages-optimized-float32-2026-10-24.md) and [optimized float64 with explicit GPU fallback](results/languages-optimized-float64-fallback-2026-10-24.md).

Earlier hardware-policy baselines on 2026-10-24:

- [Float32: hardware CPU and Metal](results/languages-hardware-float32-2026-10-24.md).
- [Float64: hardware CPU and explicit Metal software fallback](results/languages-hardware-float64-fallback-2026-10-24.md).

The [earlier software float64 comparison](results/languages-2026-10-24.md) remains a historical baseline. It predates the hardware default; its CPU and Metal floating arithmetic were emulated.

Latest GPU optimization results: [short graph batching](results/graph-batching-2026-10-26.md), with complete resident projection chains, submission counts, cancellation/resource tradeoffs and retained control results. Prior [one/two-row projections](results/skinny-projections-2026-10-26.md), [small-workload readback](results/small-workloads-2026-10-26.md), and [grouped matrix traversal](results/memory-access-pipeline-2026-10-26.md) retain their paired measurements and limitations. Earlier [strict matrix epilogue fusion](results/epilogue-pipeline-2026-10-25.md) includes complete resident/upload-inclusive timings and qualification. The subsequent [stage profile and 29 rejected microkernels](results/microkernel-investigation-2026-10-25.md) identify matrix execution as the remaining large-workload bottleneck; that investigation ships measurement tools and evidence, with no production kernel replacement. The following [input-layout and weight-packing study](results/weight-packing-investigation-2026-10-25.md) retains square gains, rectangular regressions and a rejected narrower dispatch policy, including construction/reuse/memory costs; it also ships no production replacement. Earlier [hardware float32 GPU speedup qualification](results/gpu-speedup-2026-10-24.md) retains its larger vector/matrix workloads and independent numerical checks.

The [bottleneck investigation](results/bottlenecks-2026-10-24.md) profiles checked recursive calls, CPU matrix scratch reductions and Metal device/host timing, with isolated diagnostic ablations.

Latest verified follow-up: [checked-call and matrix optimization](results/optimization-2026-10-24.md), with all source/library comparisons, cancellation-bound controls, exact matrix-cell checks and retained regressions.

Latest CPU source/runtime follow-up: [scalar tail calls and cancellation](results/tail-calls-2026-10-24.md), with all fair source results, token-bound controls, numerical qualification and retained regressions. The Rust/C++ 10% target remains unmet.

Latest source-call optimization: [bounded checked-call inlining](results/checked-inlining-2026-10-24.md), with interleaved before/after token controls, a fresh five-language comparison, differential safety/numerical qualification and the static tensor-shape correction caught by hosted documentation checks.

Latest recursive-call results: [proven call-chain contexts](results/recursive-context-2026-10-24.md), with clear/bound/observed controls, another fair source comparison, exact differential qualification, frame audits and retained validation repairs. The source-performance target remains unmet.

The [tail-return follow-up](results/tail-return-2026-10-24.md) records pure scalar token-return bookkeeping, safe signed remainder, exact baseline qualification and adjacent paired timing controls.

The [private recursive-state follow-up](results/recursive-register-2026-10-24.md) records the new default-path gains, six-depth comparison, typed correctness qualification and remaining phase/GCD gaps.
## Verified native AI sequence

The [28 October report](results/native-ai-performance-2026-10-28.md) covers the
paired CPU/GPU FFN changes, numerical-library controls, a complete transformer
block, per-cell qualification and the remaining typed GPU lifecycle gap.
The [chart](results/native-ai-performance-2026-10-28.png) includes regressions and
the MPSGraph control. Executables, IR, toolchains and frozen experiment builds
remain in ignored storage; tracked evidence contains measurements and digests.
