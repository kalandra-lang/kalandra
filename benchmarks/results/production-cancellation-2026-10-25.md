# Integrated native cancellation — 25 October 2026

Hybrid cancellation is now integrated into the production compiler and task scheduler on Darwin ARM64 (excluding ARM64e). Eligible scheduler-owned execution selects it automatically. Unsupported configurations retain cooperative cancellation; there is no source-level cancellation opt-out.

The v4 callback boundary reserves x28, captures a return point and selects clear native workers. Runtime-helper returns still check cancellation/status. The linker identifies the exact certified source-code section; runtime and scheduler frames are excluded. Each nested callback owns a new boundary above any foreign C++ owners. Physical runner identities remain protected by the execution lease and scheduler mutex. Late signals inspect current boundary/token state rather than task slots. Checked arithmetic, physical-stack checks, ownership and submitted GPU borrow lifetimes remain.

## Performance

Fresh production entries, identical source algorithms, no experimental IR rewriter or Control harness. Ordinary C++/Rust optimized numerical kernels remain the reference. Every Kalandra invocation includes state construction, checked entry, result/status/depth checks, settlement and balanced ownership checks. Two full-batch warmups precede each timed batch. Seven randomized mirrored blocks use the same iteration count per cell and verify all checksums against independent exact-arithmetic oracles. Each Kalandra result is checked within two ULP; accumulated sums use an explicit rounding bound. This measures native invocation lifecycle, not core creation or scheduler dispatch.

| Program | Depth | Hybrid / C++ | Hybrid / Rust |
| --- | ---: | ---: | ---: |
| polynomial | 10 | 0.758 | 0.715 |
| polynomial | 14 | 0.734 | 0.690 |
| polynomial | 18 | 0.739 | 0.690 |
| polynomial | 22 | 0.735 | 0.685 |
| branches | 10 | 0.820 | 0.815 |
| branches | 14 | 0.800 | 0.802 |
| branches | 18 | 0.814 | 0.812 |
| branches | 22 | 0.818 | 0.811 |
| euclid | 10 | 0.978 | 0.820 |
| euclid | 14 | 0.956 | 0.740 |
| euclid | 18 | 0.985 | 0.747 |
| euclid | 22 | 0.987 | 0.760 |
| chain | 0 | 9.679 | 4.838 |
| chain | 100 | 1.059 | 1.052 |
| chain | 1000 | 1.030 | 1.040 |
| chain | 10000 | 1.041 | 1.032 |

**30/30 non-empty comparisons meet the 10% target by median.** The depth-100 chain is 5.9%/5.2% slower; all seven depth-100 block comparisons are also within 10%. Empty entry costs 17.40 ns versus 13.36 ns for the production unbound entry and 1.80/3.60 ns for ordinary C++/Rust calls. The empty entry still has material task-lifecycle overhead. Timing medians are not worst-case guarantees or proof about every program.

## Qualification

- 101 distinct focused CTest gates pass (85 broad regression gates plus 20 targeted gates with four overlaps), including strict scalar/Bool/C-return numerics, signed remainder, checked inlining, casts, call foundations, fast/tail calls, closures, records, strings, tasks, ownership and source execution.
- All 12 boundary configurations pass after the final adversarial-register/handler-ownership checks: debug, optimized, ASan/UBSan and TSan, each with normal, pre-owned and blocked signal configurations.
- 2,400 nested scheduler cancellations validate cleanup, deadlines, faults and forced migration. The three actual-async configurations additionally complete 3,000 fresh invocations while stale signals are injected. x28 is deliberately borrowed by a foreign helper and verified restored.
- Production-generated programs pass 216 exact numerical checks and 480 scheduler cancellations across four build configurations. Debug/optimized/ASan record actual PC redirects; TSan explicitly records cooperative fallback.
- Benchmark verification passes all 300 oracle cases across three seeds, five depths, four programs and five implementations/modes; 1,120 timed samples retain verified checksums.
- The physical M2 Max Metal check forces 100 actual hybrid CPU redirects while event-gated GPU commands are already submitted, settles each CPU invocation, and then verifies 409,600 exact GPU values. Metal is initialized after signal ownership is acquired, so this also checks framework coexistence on this Mac.

## Supported behavior and limits

The runtime reserves SIGUSR2 only when its disposition is default, keeps it through process exit, and never replaces an existing owner. Workers already blocking it use polling. Foreign code cannot replace that private disposition, block it inside an active lease, unload the runtime or inline foreign C++ owners into the certified section. This is the private standalone-runtime contract; it does not introduce public embedding or arbitrary foreign interruption. Runtime-helper/GPU external lifetimes must finish before storage is reclaimed.

The initial forced-async TSan qualification timed out. TSan defers asynchronous handlers and supplies a copied context; editing that copy cannot redirect the live thread and its saved boundary may become stale. TSan therefore remains instrumented and cooperative. That failed attempt is retained in the qualification record, not relabeled as an async pass. The handler itself has no sanitizer callbacks, allocations, locks or cleanup.

These measurements and physical execution are on Apple M2 Max / macOS 27 with pinned LLVM 23.1.3_2. The configured deployment minimum remains macOS 26. Physical macOS 26 and hosted Linux/Windows execution are not established by this local run; those targets retain their existing cooperative implementations. No blanket whole-repository or universal-performance qualification is claimed.

- [Performance samples, oracle checks, commands and hashes](production-cancellation-2026-10-25.json)
- [Qualification records, source hashes, GPU evidence and retained failed attempt](production-cancellation-qualification-2026-10-25.json)
- [Native call contract](../../docs/design/checked-call-optimization.md)
- [Production entry runner](../../native/experiments/cancellation/production_entry.py)
- [Earlier runtime-entry correction](cancellation-runtime-entry-2026-10-25.md)

The initial broad helper barrier also caused a debug source execution timeout. Narrowing it to task/ownership helper edges restored normal execution (92.79 seconds on the final rerun); the final 101 distinct gates pass. Those failed attempts are retained in the qualification record.
