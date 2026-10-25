# Cancellation architecture experiments — 25 October 2026

The hybrid prototype combines asynchronous interruption of eligible generated code, checks when runtime helpers return, and a direct transfer to task-ledger cleanup. It removes per-call polling from pure compute without relying on eventually sampling a short native interval between allocations. In this run its paired time was 96.2–101.6% of the unbound control; all twelve equivalent-source comparisons met the 10% target against ordinary C++/Rust. Production cancellation is unchanged.

This implements the proposed compiler-arranged cleanup per task. Detection and cleanup transfer are separate choices: targeted signals, ordinary polls or a page fault can all reach the same cleanup boundary. The experiment does not force-kill an OS thread.

The [follow-up depth study](cancellation-depth-scaling-2026-10-25.md) extends trees to depth 22 and adds genuine non-tail chains at 100/1,000/10,000 levels. It identifies a short-chain exception: depth 100 misses the 10% target (27% slower than C++, 25% slower than Rust), while the deeper chains meet it. The earlier twelve-cell result below remains limited to its measured tree workloads.

## Measured mechanisms

Apple M2 Max, macOS 27, Darwin ARM64; existing pinned LLVM/compiler and optimized runtime. Three adjacent ABBA/BAAB blocks per algorithm/depth/candidate, twenty warmup tasks per process, approximately 25 ms measured batches. Per-task state allocation, entry boundary and ledger settlement are timed. The unbound floor uses the same heap-based probe harness. The current bound path uses the actual compiler/runtime token mode. Candidates reserve x28 and use checked linked PC metadata; callback adapters reinstall the register even if ordinary runtime code borrows it.

Throughput ratios are candidate time / unbound time across three algorithms and depths 10/14; 1.00 is equal. The range retains all eighteen paired samples per mode. Latency is request through **completed cleanup**, rather than signal arrival or acknowledgement. Each latency row contains 200 independent worker invocations; cancellation workers have 8 MiB stacks.

| Mode | Median ratio | Full paired range | Polynomial cleanup p50 / p95 (µs) | Allocation-heavy cleanup p50 / p95 (µs) |
| --- | ---: | ---: | ---: | ---: |
| `current` | 1.948 | 1.599–2.743 | 39.333 / 46.542 | 4953.541 / 5872.667 |
| `entry` | 1.006 | 0.984–1.377 | 0.167 / 0.416 | 79.292 / 95.125 |
| `cycles` | 1.008 | 0.987–1.347 | 0.167 / 0.500 | 81.167 / 94.375 |
| `budget` | 1.138 | 0.994–2.766 | 0.458 / 0.834 | 83.750 / 102.042 |
| `guard` | 1.012 | 0.991–1.363 | 0.208 / 0.750 | 82.625 / 96.958 |
| `page` | 1.178 | 1.002–2.180 | 24.791 / 31.542 | 108.417 / 123.375 |
| `async` | 0.995 | 0.975–1.015 | 4.000 / 10.084 | 241.500 / 799.417 |
| `async-unwind` | 1.003 | 0.981–1.034 | 78.583 / 97.917 | 22217.750 / 60203.250 |
| `hybrid` | 1.004 | 0.962–1.016 | 3.708 / 8.042 | 90.417 / 115.833 |
| `unwind-poll` | 1.005 | 0.971–1.354 | 67.542 / 78.292 | 11929.041 / 13533.583 |

The identical-placement `entry`/`unwind-poll` control isolates cleanup transfer: direct settlement avoids unwinding thousands of native frames. Differences in the number of allocations reached before a request remain intrinsic to throughput and scheduling, so allocation-heavy latency is not a fixed-owner-count microbenchmark. The original token path also carries a different internal call convention; its throughput difference cannot be attributed to exception handling alone.

A local budget still loads/stores a counter on each visit and regresses selected cases. Fusing cancellation into stack checks helps growing recursion but still needs loop coverage. Protected pages require a per-task mapping and incur VM/fault costs. Async-only execution has nearly unbound throughput but longer allocation-heavy cancellation latency because runtime code is deliberately ineligible. The hybrid retains that compute throughput and checks deterministically at helper returns.

## Equivalent-source comparison with ordinary C++ and Rust

The same polynomial, branching and Euclid source algorithms are used in every language. No tensor built-ins, matrix operations or optimized numerical libraries replace the recursive algorithms. C++ uses Clang `-O2` with contraction disabled in the numerical translation unit; Rust uses `opt-level=2`, without LTO. Neither reference implements cancellation. Every measured and warmup checksum matches the independent exact reference. Three ABBA/BAAB blocks use identical iteration counts within each pair.

| Algorithm | Depth | Hybrid / C++ time | Hybrid / Rust time |
| --- | ---: | ---: | ---: |
| polynomial | 10 | 0.795 | 0.764 |
| polynomial | 14 | 0.737 | 0.697 |
| branches | 10 | 0.845 | 0.846 |
| branches | 14 | 0.796 | 0.797 |
| euclid | 10 | 0.987 | 0.850 |
| euclid | 14 | 0.987 | 0.775 |

All twelve cells meet the requested 10% target in this narrow experiment. These are experimental entry boundaries, not an integrated production scheduler, and do not establish language-wide performance. Earlier pre-callback-adapter runs included a Euclid cell 2.6% slower than C++; small differences near parity should not be treated as stable wins.

## Correctness and failures

- 891 exact numerical workloads across eleven modes, six programs, multiple seeds and depths.
- 12,000 long-running cancellations; every invocation cancelled and released its owned ledger.
- 6,000 completion-race trials; only cancellation or complete, correct results were accepted.
- 44 injected allocation failures retained the expected allocation fault and balanced ownership.
- 108 adversarial callback-register checks, including 162 extra long cancellations. Runtime-like assembly deliberately borrows x28 across a callback; the adapter reinstalls the task register and preserves the foreign caller’s value.
- 15,734,636 allocation/release pairs checked across numerical, cancellation, race, allocation-failure and callback tests, excluding timed throughput samples.
- ASan/UBSan: all ten cancellation modes passed 100 allocation-heavy cancellations each; raw stderr is empty.
- TSan: eight modes passed 50 allocation-heavy cancellations each, including hybrid and direct polling. **Async-only and async-unwind failed this stress qualification**: both reached physical-stack fault 6 before interruption, with all 7,281 owners released. No race diagnostic was suppressed; their failures are retained in the raw evidence.
- Physical Metal: 100 event-gated submissions and 409,600 exact output values. CPU ledger settlement occurred while the submitted command waited; the final borrow lease remained alive until GPU completion. This verifies the ownership primitive, not integrated task cancellation of an entire GPU graph.

The first prototype missed loops in optimized boundary wrappers. The call-free test exposed this; final preparation includes both generated bodies and wrappers. Initial allocation stress used 512 KiB worker stacks and reached the existing physical-stack fault before cancellation. The final 8 MiB qualification avoids confusing that fault with successful cancellation. ThreadSanitizer’s larger frames still expose the async-only limitation.

## Safety boundary and implementation decision

Proceed with the hybrid cleanup-boundary architecture as the leading implementation candidate. Keep the production default until its scheduler and platform integration satisfies the contracts below. The experiment has already compared direct cleanup, normal unwinding, sparse polling, budgets, guard fusion, protected-page polling and asynchronous mechanisms; this decision is based on implemented alternatives, not implementation-effort estimates.

Direct frame abandonment is legal here because the skipped generated frames, entry adapters and source boundary have no nontrivial C++ destructors, and owned backing is in the invocation ledger. A signal handler only validates PC/state and redirects saved execution state. It returns normally before the bridge transfers control or throws. C++ allocator/runtime critical regions are excluded; hybrid checks occur after they return. Ordinary runtime code is allowed to borrow the reserved register, so it must be reinstalled when reentering generated code.

Passing sampled async unwinds does not prove that every prologue/epilogue has valid unwind state. Whole-function PC spans are not sufficient proof for arbitrary asynchronous C++ unwinding. Arbitrary foreign callbacks may have live destructors below generated code and require a critical-depth/callback contract. No source frame may be abandoned while it owns an unregistered external resource. Submitted GPU operations retain their buffers through completion.

Production work remains: task generation and late-signal handling on pooled workers; scheduler cancellation/fault/result-publication ordering; migration and nested entries; explicit foreign/runtime critical regions; process-wide handler coexistence and teardown; resource registration for future FFI; diagnostic/sanitizer stack behavior; Linux/Windows mechanisms; ARM64e pointer authentication; execution qualification on minimum macOS 26. OS delivery and blocking foreign calls do not provide a real-time cancellation bound. None of these gaps is marked complete by this experiment.

## Evidence and reproduction

- [Mechanism samples and hashes](cancellation-mechanisms-2026-10-25.json)
- [C++/Rust paired samples and commands](cancellation-hybrid-fair-2026-10-25.json)
- [Sanitizer stdout, stderr, failures and hashes](cancellation-sanitizers-2026-10-25.json)
- [Physical Metal lifetime result](cancellation-metal-borrow-2026-10-25.json)
- [Sources, safety contract and runnable commands](../../native/experiments/cancellation/README.md)
