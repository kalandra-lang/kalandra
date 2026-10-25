# Native call foundation

Synchronous source calls now return native values, pass closure environments directly and propagate fatal execution failure to one checked runtime boundary. They no longer publish a logical recursion depth or return a success bit on every edge. The arbitrary 128-call execution cap is removed. Eligible self, mutual, indirect and aggregate tail calls reuse their frame; non-tail recursion is checked against the executing thread’s physical stack.

This changes compiler/runtime internals without adding ownership or mutation syntax. Immutable source values, typed application errors, checked arithmetic, cancellation, fault coordinates and invocation-owned cleanup remain part of the contract. See [the implementation contract](../../docs/design/checked-call-optimization.md).

## Mechanism and limits

- Native scalar returns and explicit caller-provided aggregate result storage replace checked value/status packets on internal edges. Function values retain their checked code identity for worker transport and validation.
- Mode-specific workers retain clear, token-bound and observed execution. Bound tokens use acquire loads; observed mode retains runtime probes. Checked helpers still stop immediately on failure.
- A private native abort marker unwinds deeper failures to a C++ runtime boundary. Inlined failure exits in the outer callback return failure directly. Frame pointers and unwind tables are required. No source-level exception API is added.
- Win64 scalar tail-call components use equal-sized private argument packets so unwind tables remain valid; public checked C signatures are unchanged.
- LLVM `musttail` handles eligible forwarded returns. Darwin ARM64 large argument packets retain caller-pop calls because the pinned backend corrupts their callee-pop form at `-O0`; matching signatures still support guaranteed tail transfer. Frame-local allocations conservatively prevent tail forwarding.
- Physical stack admission reserves 128 KiB. Non-tail source edges check the stack pointer; the conservative per-function budget remains 16 KiB. The current task-planned/fallible and ownership traversal engines retain their separate checked boundaries; this is not a promise of constant space for every recursive path.
- General control-flow leaf outlining, including pure loops and terminal fault paths, removes the former base-case frame cliff. Inlined call guards are removed only after proving the accepted continuation cannot grow a source frame. It does not unroll to a benchmark-selected recursion depth. Numerical algorithms, operation order and compiler arithmetic policies are unchanged.

Qualification also repaired resolver composition for task-produced closures, an exposed signed narrow C-return extension bug, shared aggregate result scratch slots to preserve acceptance of long tensor programs, and excluded intrinsic metadata from physical argument accounting. A shared execution-context experiment was discarded after regressing the arithmetic-heavy cases.

## Final measurements

Code checkpoint: `0874646e`; compiler SHA256 `065bb0db4c5dc9fed067f72b66a96e8511481d01843931dd43586bf0807d01bd`. Apple M2 Max, native ARM64, strict floating-point order. Builds and tests finished before timing. Inputs, algorithms and numerical references are unchanged.

The [fresh five-language comparison](fair-source-call-foundation-refined-2026-10-25.md) meets the requested 10% Rust/C++ target for **all 18 measured unbound source cases**: three algorithms at depths 9–14. Polynomial is 20–26% faster than C++, branching recursion 14–20% faster, and Euclid ranges from 2% faster to 1% slower. All 18 also beat Rust in this run. This is qualification of this source suite, not a language-wide performance guarantee.

| Algorithm, depth 14 | Kalandra µs | C++ µs | Rust µs | Go µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| polynomial | 28.861 | 39.047 | 41.757 | 35.262 | 2109.005 |
| branches | 39.715 | 49.063 | 49.928 | 52.736 | 2403.049 |
| euclid | 120.488 | 119.442 | 157.760 | 161.215 | 7090.146 |

Three fresh processes per cell, two warmups and seven measured batches; every checksum is checked against independent integer/Fraction/GCD references. Setup/startup is excluded; normal invocation, checked arithmetic and physical-stack admission are included. These programs use equivalent source recursion in every language, with no tensor or numerical-library substitutions. General GPU source recursion remains unsupported and is not replaced with a different algorithm.

The separate [paired before/after comparison](call-foundation-refined-paired-2026-10-25.md) uses nine adjacent ABBA/BAAB process blocks, six depths and clear/bound/observed modes. Each compiler links its own frozen runtime. At depth 14 in the unbound control:

| Algorithm | Before µs | After µs | Time change |
| --- | ---: | ---: | ---: |
| polynomial | 26.595 | 28.752 | +8.1% |
| branches | 30.677 | 39.502 | +28.8% |
| euclid | 414.680 | 125.494 | −69.7% |

**Retained regressions:** the old compiler wins at several expansion phases. Unbound depth-11 polynomial/branches regress about 56%/64%; cancellation-bound polynomial regressions reach about 38%. The new implementation removes the old phase cliff and arbitrary call cap, but is not universally faster than that compiler. Cancellation-bound and observed results are retained in the complete paired table; the cross-language target above applies to the disclosed unbound suite.

The intermediate [foundation-only paired run](call-foundation-paired-2026-10-25.md) and [five-language comparison](fair-source-call-foundation-2026-10-25.md) show Euclid's remaining gap before removing obsolete inlined stack guards and adding AArch64 `SDIV`/`MSUB` remainder lowering. The latter preserves zero-divisor faults and the defined zero result for `INT_MIN % -1`; other targets retain portable checked lowering. No arithmetic safety checks or floating-point policy were disabled.

## Qualification

The [final qualification record](call-foundation-qualification-2026-10-25.json) records **104 distinct focused CTest gates**, all passed after the dedicated physical-Metal rerun, with compiler/source/runtime hashes and individual attempts. It includes:

- Million-step self, mutual, indirect and captured tail calls; aggregate and wide-argument calls; deep non-tail faults, cancellation, physical-stack exhaustion and balanced cleanup in debug, optimized, ASan/UBSan and TSan modes.
- 571,272 executed scalar/Bool/narrow-C-return checks across variants, plus 2,367,960 signed-remainder/state checks. Raw C returns are promoted before comparison so narrow extension errors cannot hide.
- Recursive records, shared DAGs, malformed cycles, closures, transported callables, coordinating tasks, arrays, typed errors, numerical tensors, static shapes, hardware conversions, matrix cells and allocation-failure sweeps.
- Physical Metal numerical optimization and matrix checks, capability probes, and identical supervisor artifacts from normal and sanitizer-instrumented compilers using identical arithmetic flags.
- All 63 rendered documentation blocks and 39 native examples across 20 Tiramisu pages, including 32 CPU and two physical Metal programs. Windows-only setup and environment installation recipes are reported separately by the verifier.

A broad **earlier checkpoint** sweep ran 1,111 of 1,353 registered tests: 1,030 passed and 81 failed. Those failures drove the fixes and updated expectations described here. That sweep is diagnostic history; its successes do not qualify the final compiler. The final focused selection is explicit and is not described as a full-suite pass.

The [physical-frame audit](call-foundation-refined-frames-2026-10-25.json) checks 164 ARM64 binaries and 4,300 emitted frame records, including instrumented code. Maximum conservative frame size is **6,352 bytes**, below the 16 KiB model and 128 KiB runtime reserve. This is evidence for these binaries, not a proof for all programs. The [frame-entry counters](call-foundation-refined-counts-2026-10-25.json) find no depth-zero polynomial worker entries at depths 9–14; the old depth-12 base-case frame cliff is gone. Instrumented timings are never used as performance evidence.

## Hosted status at report writing

These runs qualify code checkpoint `0874646e`; the final evidence/documentation commit leaves compiler inputs unchanged. [Windows qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40150158232) is successful, including scalar/deep-call execution, reproducible packaging and downloaded-package acceptance. [Linux qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40150158221) has passed its call/scalar/remainder and documentation checks; independent distribution rebuild/acceptance is still running. [macOS qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40150158229) is running after an obsolete checkpoint's notarization job was cancelled to release the workflow slot. Its final build/signing/notarization result is not yet claimed. Earlier hosted successes do not qualify this code checkpoint.

## Remaining scope

Task-planned/fallible and ownership traversal paths retain separate checked runtime boundaries. Heap-based traversal, broader tail-call coverage and tighter frame estimates remain work. Cancellation-bound polynomial regressions remain visible optimization work. The measured CPU source target does not establish GPU recursion support, GPU speedups, general library performance or parity for every language feature.

