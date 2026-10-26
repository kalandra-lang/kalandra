# Kalandra native benchmarks

Use the [native benchmark guide](../native/benchmarks/README.md) for CPU kernels, scheduler overhead, tensor shapes and Apple Metal measurements. The harnesses live in `native/benchmarks`; raw samples, commands, source hashes and reports live in `benchmarks/results`.

Latest source-call results: [range proofs, bounded host entry and native checkpoint](results/range-foundation-2026-10-26.md). Both repeat runs place all six nonempty array/closure cases within 3.1% of C++/Rust; larger array cases take 35–45% less time. The 30 nonempty recursion comparisons remain within 8.1%. Zero-round host-entry controls still trail substantially and remain visible in the report.

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
