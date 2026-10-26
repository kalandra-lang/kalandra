# Native range, ownership and checked-entry optimization

The minimum target passes on the M2 Max for all six nonempty array/closure cases in two longer runs: the worst Kalandra/reference time ratio is 1.031. The refreshed production recursion study has 30 nonempty C++/Rust comparisons; its worst ratio is 1.081. These are measured corpus results, not a universal language or cross-platform performance guarantee.

Array cases at 64 and 256 rounds show material wins over both references. Long scalar closures remain around parity. Small closure differences of a few percent are not evidence of a general advantage.

## Cross-language results

Numbers below are per invocation in nanoseconds from the first 50 ms calibration run. Ratios below 1 mean less elapsed time. The last column gives the worst ratio against either reference in the independent repeat.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust | Repeat worst |
|---|---:|---:|---:|---:|---:|---:|---:|
| Immutable array | 16 | 412.35 | 420.38 | 425.86 | 0.981 | 0.968 | 0.987 |
| Immutable array | 64 | 1163.00 | 1777.57 | 1799.03 | 0.654 | 0.646 | 0.643 |
| Immutable array | 256 | 4049.83 | 7311.66 | 7015.12 | 0.554 | 0.577 | 0.584 |
| Captured scalar closure | 16 | 25.95 | 26.34 | 26.25 | 0.985 | 0.989 | 0.960 |
| Captured scalar closure | 256 | 915.50 | 888.01 | 896.15 | 1.031 | 1.022 | 1.019 |
| Captured scalar closure | 4096 | 14852.18 | 14882.34 | 14790.36 | 0.998 | 1.004 | 1.018 |

Zero-round controls remain expensive: the array entry is about 108 ns versus 21 ns, and the scalar entry about 13 ns versus 4.1 ns C++ / 1.4 ns Rust. They retain invocation/marshalling/cleanup cost with no recurrence work; the array case also constructs its initial tensor. They are retained in the raw files and are not included in the nonempty-work acceptance statement. Ordinary internal Kalandra function calls do not construct a fresh invocation per call.

## Changes relative to the frozen baseline

The baseline is commit c12d11c2. Nine adjacent ABBA/BAAB blocks use identical iterations per pair, two warmups and three retained rounds per process, at 50 ms calibration. Every invocation checks its result. Median paired elapsed-time reductions:

| Program | Rounds | Time reduction |
|---|---:|---:|
| arrays | 16 | 36.50% |
| arrays | 64 | 44.37% |
| arrays | 256 | 47.46% |
| closure | 16 | 46.72% |
| closure | 256 | 9.96% |
| closure | 4096 | 8.87% |

The complete scheduled-scope diagnostic is essentially unchanged within noise; it includes pool/monitor construction, spawn, await and destruction. Its fixed-input synchronous companion improves strongly for the tiny constant case, which is not a cross-language or steady-state scheduler ranking.

## Architecture and correctness contract

- Dominator/value-range proofs remove only established redundant integer comparisons. Future assumptions, changing PHIs, sibling guards, memory reloads and wrapping ranges cannot justify a fold.
- Positive structural array countdowns use a peeled counted loop; negative counts retain checked while-loop behavior. Bounded compiler deallocation workspaces use nonescaping stack storage. Immutable aliases, changing views, logical allocation failures and byte quotas remain checked.
- Clear workers can discard a promoted allocation’s obsolete status branch only after an exact ABI/context/effect proof. Watched/cooperative workers retain their checks.
- A separate bounded scalar host entry checks cancellation before/after at most 256 body IR instructions. Admission uses a bounded projection and at most eight small-integer field guards. Nested, watched, faulted, settled, armed or unproved work retains v4. The code is not admitted for asynchronous source-PC redirection.
- Guarded input ranges and an inductive nonnegative-remainder proof enable exact arithmetic narrowing. Larger/negative inputs retain the original implementation. INT32_MIN / -1 and INT32_MIN % -1 retain their correct int64 semantics. Source types, overflow/fault order and strict floating point are unchanged.
- Immutable argument fields remain SSA values across acquire cancellation polls instead of being reloaded from an opaque callback packet.
- Empty-ledger settlement is inline, with full cleanup for owned/cache/budget resources. Live-count diagnostics use single-writer atomic loads/stores; shared backing reference counts retain their synchronization. Runtime helpers reuse poll-mode health state while preserving cancellation loads and the observed path.
- Darwin ARM64 production cancellation saves/restores its recovery checkpoint in the native entry trampoline, including all ABI callee-saved GPR/FP registers. Cooperative entry remains separate; ASan retains sanitizer-supported sigsetjmp/siglongjmp recovery.

No source algorithm substitution, fast-math, unchecked mode, numerical-library substitution, benchmark-only production branch or weakened reference implementation was used. C++ and Rust retain ordinary optimized generics/templates. Default constructor inlining was tested and reverted; standalone lifecycle/layout/QoS experiments are not counted as wins.

## Validation

The full 1,519-step build passes. All 417 distinct selected CPU gates and 50 selected Metal device/codegen gates pass. CPU qualification includes all 12 native cancellation debug/optimized/ASan+UBSan/TSan stress variants, compiler proof units and source-language regression fixtures. The focused source suite checks 192,560 exact integer recurrence/division/value/fault cases across debug, optimized, optimized ASan/UBSan and optimized TSan builds, plus ownership, immutable-alias, float-bit, cumulative budget, allocation-failure, stack-admission, cancellation and recovery tests. Register sentinels cover x19–x28 and d8–d15 through actual native cancellation redirects. These are selected gates, not a claim that every registered CTest was rerun.

Independent audits recompute the paired integer oracle with modular exponentiation, rebuild the reported statistics and verify measured artifact hashes. The recursion study retains 300 Fraction-oracle verification workloads and 1,120 timed samples. Timing runs do not overlap builds, tests or profiling. There is no CPU affinity, frequency, QoS or desktop isolation; small differences require caution.

## Evidence

- [Qualification, selections and source/evidence hashes](range-foundation-qualification-2026-10-26.json).
- [Full comparison](runtime-workloads-range-final-2026-10-26.json) and [repeat](runtime-workloads-range-confirmation-2026-10-26.json).
- [Paired before/after](range-foundation-paired-2026-10-26.json) and [independent audit](range-foundation-paired-audit-2026-10-26.json).
- [Production recursion refresh](production-cancellation-range-final-2026-10-26.json).
- [Profiles](range-foundation-profile-2026-10-26.json) and [evidence/artifact audit](range-foundation-evidence-audit-2026-10-26.json).

The next opportunities are broader range specialization in long-running native workers, generated armed-mode helper-status bridges, managed reservation/accounting and fixed host-entry cost. Array samples show that actual malloc is rare through buffer reuse; allocation/free policy and numerical work remain visible. Material wins here do not establish that every Kalandra program beats an equally tuned C++/Rust implementation.
