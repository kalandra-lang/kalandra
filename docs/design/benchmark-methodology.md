# Benchmark comparison requirements

The existing `native/benchmarks/languages` suite compares the same mathematical results, not equivalent compiler workloads. It remains useful for regression measurements of the recorded implementations. It does not establish that Kalandra is generally faster than Go, Rust or C++, or that the requested 10% compiler-performance target has been met.

## Audit finding

Kalandra calls built-in tensor maps, balanced reductions and `matmul`. Its compiler can fuse maps and reductions, omit intermediate arrays, vectorize loops and select tiled matrix kernels. The C++, Go and Rust references explicitly materialize mapped arrays; AXPY materializes a scaled array too. Their matrix implementations compute each cell with a scalar product loop and an explicit product buffer/reduction, without Kalandra's matrix tiling strategy. Python uses interpreter loops without a numerical library. Numerically identical answers do not remove these algorithm, allocation and implementation differences.

The reported times and independent numerical checks remain evidence about those specific programs. The earlier conclusion that they qualified general Rust/C++ performance parity is withdrawn. The historical reports retain their raw measurements and now carry a correction. GPU-versus-Kalandra-CPU results remain valid measurements of Kalandra's two tensor backends, with the documented timing boundaries and single-thread CPU baseline; they do not compare GPU performance with optimized ecosystem libraries.

## Source-level compiler workloads

A replacement compiler comparison must use application algorithms expressible in each source language, excluding built-in matrix/reduction operations and external numerical kernels from the measured work. Examples include scalar computation, control flow, recursion, indexing, parsing and traversal. Document actual source-level iteration/recursion support instead of substituting a native built-in for a missing control-flow feature.

For each case, disclose and align the algorithm, data representation, arithmetic/overflow rules, allocation and copying boundaries, threading, compiler settings and observable outputs. Different control-flow mechanisms or runtime checks must be reported rather than silently called equivalent. Include source and generated-code inspection, independent correctness references, cold/warm distinctions where relevant, and the existing repeated-process sampling protocol. A handful of cases still cannot establish a language-wide ranking.

## Numerical implementation and ecosystem workloads

Keep built-in tensor performance as a separate benchmark. Compare it with competent numerical implementations available to each ecosystem, including optimized library-backed operations where available. Disclose the actual backend: several language wrappers around the same BLAS measure that library and wrapper overhead, not independent language kernels. Include native tensor graphs/fusion workloads where they answer an application question.

Align precision, dimensions/layouts, hardware, CPU thread counts, allocation boundaries, input residency and transfer costs. Compare both controlled single-thread and explicitly configured multi-thread CPU libraries. Separate GPU and CPU capabilities clearly. Preserve results per workload and backend, including cases Kalandra loses.

Library arithmetic may use FMA or a different summation tree. Such comparisons need an independently justified numerical accuracy criterion and must disclose the contract difference. Keep Kalandra's existing bitwise/FTZ-aware conformance tests intact; do not relax its language semantics merely to match a library.

## Status

The audit and claim corrections are complete. The replacement [fair comparison suites](../../native/benchmarks/fair/README.md) implement equivalent source recursion/control-flow workloads and optimized OpenBLAS/NumPy/Accelerate numerical API workloads. Each numerical operation includes Kalandra CPU and resident/upload-inclusive Metal variants. General source recursion has no GPU score because that execution path is unsupported.

The [measured results and limitations](../../benchmarks/results/fair-benchmarks-2026-10-24.md) retain the losses, thread modes, allocation differences, numerical error criteria and raw samples. They supersede the earlier compiler-parity claim; these narrow workloads do not qualify a language-wide performance target.

The [checked-call/matrix follow-up](../../benchmarks/results/optimization-2026-10-24.md) reruns both fair suites after optimization. It preserves the same algorithms, library APIs, arithmetic and timing boundaries. The Rust/C++ source-performance target remains unmet; faster tensor kernels are not used as evidence of compiler parity.

The subsequent [scalar tail-call and cancellation results](../../benchmarks/results/tail-calls-2026-10-24.md) retain all source algorithms, intermediate variants, token costs and small regressions. Numerical and safety qualification accompany the final compiler. These measurements still do not meet the Rust/C++ source-performance target.

The [bounded inlining results](../../benchmarks/results/checked-inlining-2026-10-24.md) retain the same source algorithms and add an interleaved baseline/current control using one byte-identical runtime archive, with bound/unbound cancellation tokens. The initial trial and final qualification are separate records; the source-performance target remains unmet.

The [recursive context results](../../benchmarks/results/recursive-context-2026-10-24.md) retain source/precision semantics and include an observer-bound control. Diagnostic guard ablations are explicitly invalid language implementations. Initial scheduling and packaging-fixture failures, their corrections and affected reruns remain in the qualification record. The current target gap is reported alongside the gains.
