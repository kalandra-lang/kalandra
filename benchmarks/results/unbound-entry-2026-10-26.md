# Contract-aware native entry — 2026-10-26

Ordinary standalone execution already starts without a cancellation token. The
previous fair entry harness explicitly selected cancellable embedding. This pass
adds compiler-proven unbound entry and measures both contracts separately. The
cancellable harness remains the default; no task token is removed from running
cancellable code.

Nine matched ABBA/BAAB blocks against frozen native commit `005e02d7`, with an
identical ordinary harness, show **68.45% less elapsed time for zero-round scalar
closure entry**. Its full-suite values are **2.94 ns**, with **2.71 ns** in the
repeat, versus **9.15 ns** in the ordinary baseline. This reduction comes from
compiler-generated code, rather than changing the cancellation contract between
baseline and candidate.

All nonempty source cases have paired median time changes within **0.6%** of the
baseline. The corrected short-loop case has a 0.33% median reduction, retaining
a losing block of 9.81%. Array zero-round median time changes by -0.50% (slower),
with all blocks retained; allocation and ownership remain necessary.

**The 10% zero-round C++/Rust target remains unmet.** Ordinary scalar
entry beats C++ in these runs but still trails Rust. Array zero-round entry
remains about twice the reference cost. Unknown callers still require an
execution-mode guard, and checked arithmetic/fault handling remains enabled.

## Fair comparisons

The first ordinary full run puts all six nonempty cases within **7.51%** of
C++/Rust or faster. The independent repeat's worst gap is **0.87%**. Under the
original fully cancellable contract the worst nonempty gap is **0.99%**. These
are host measurements, not universal superiority claims.

The production recursion refresh retains 300 independently checked verification
workloads and 1,120 timed samples. With a shared 100 ms calibration, all 30
nonempty hybrid/C++/Rust comparisons remain within **5.80%**. Non-tail depth 100
is 847.28 ns versus C++ 800.87 ns and Rust 802.52 ns; depths 1,000 and 10,000
also remain within the target. Ordinary and cooperative measurements remain in
the data, including losing samples.

### Ordinary full run

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust |
|---|---:|---:|---:|---:|---:|---:|
| Immutable array | 0 | 45.08 | 22.20 | 22.80 | 2.031 | 1.978 |
| Immutable array | 16 | 343.07 | 435.18 | 403.95 | 0.788 | 0.849 |
| Immutable array | 64 | 1058.83 | 1867.66 | 1747.49 | 0.567 | 0.606 |
| Immutable array | 256 | 4099.50 | 7741.39 | 7274.33 | 0.530 | 0.564 |
| Captured closure | 0 | 2.94 | 4.47 | 1.49 | 0.658 | 1.973 |
| Captured closure | 16 | 24.52 | 29.69 | 28.62 | 0.826 | 0.857 |
| Captured closure | 256 | 894.10 | 907.33 | 893.39 | 0.985 | 1.001 |
| Captured closure | 4096 | 15971.97 | 14864.87 | 14857.16 | 1.074 | 1.075 |

### Cancellable full run

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust |
|---|---:|---:|---:|---:|---:|---:|
| Immutable array | 0 | 45.99 | 22.32 | 22.52 | 2.061 | 2.042 |
| Immutable array | 16 | 358.61 | 458.06 | 432.49 | 0.783 | 0.829 |
| Immutable array | 64 | 1109.95 | 1836.63 | 1910.45 | 0.604 | 0.581 |
| Immutable array | 256 | 4220.48 | 7628.88 | 7270.74 | 0.553 | 0.580 |
| Captured closure | 0 | 9.93 | 4.06 | 1.36 | 2.444 | 7.321 |
| Captured closure | 16 | 24.42 | 26.72 | 26.25 | 0.914 | 0.930 |
| Captured closure | 256 | 899.62 | 890.87 | 897.12 | 1.010 | 1.003 |
| Captured closure | 4096 | 14841.33 | 14883.19 | 14898.05 | 0.997 | 0.996 |

The ordinary repeat retains all eight rows in its raw data. Task-scope
diagnostics include complete scope/pool/monitor creation and settlement and are
Kalandra-only; fixed-input direct diagnostics can constant-fold and do not rank
languages.

## Compiler and safety contract

The compiler specializes a closed callback graph for a healthy, unobserved
invocation without a token. It admits only initialized packet reads, private
packet writes and supported native arithmetic. Allocation, foreign/indirect
calls, task operations, remaining stack guards, stack allocations and
atomic/volatile operations retain the general entry. Faults keep their original
code/location and end the callback locally. The optimized path avoids runtime
admission, native cancellation setup, depth accounting and resource
ledger materialization. It does not need a loop cancellation-latency budget.

Existing bounded/range specializations run first. Their side-effect-free argument
guard runs before reading control/depth so an ineligible packet can reach the
unbound path directly. Token-bound, observed, faulted and settled invocations
keep their existing behavior. The runtime implementation and private ABI layout
are unchanged.

The initial ordering bypassed short-loop narrow arithmetic: its first unpaired
ordinary run showed a 28.6% scalar-16 regression. That ordering was rejected;
its full data, source snapshots and independent audit remain linked below. The
final matched study confirms the regression is gone. No losing blocks were
removed.

## Validation

The full native build and **419 selected CPU + 50 physical Metal/device-codegen
gates pass**. The source suite verifies **192,560 exact numeric results and
first-fault cases** from poisoned invocation storage across debug, optimized,
ASan/UBSan and TSan builds. Optimized probes check that ordinary scalar entry
leaves managed control/resource state cold, while pre-faulted and settled
contexts retain their behavior. All 12 native cancellation stress variants
pass. Coverage includes ownership/aliases, allocation failures, cumulative
budgets, strict float bits, nested calls and cancellation/recovery. There are
17 bounded and 24 unbound proof admission/rejection cases.

Independent closed-form modular/Fraction audits validate checksums, statistics
and measured artifact digests. The source algorithms, timing protocol and
C++/Rust reference programs are unchanged. Baseline and candidate use exactly
the same ordinary driver; execution-contract mismatch is rejected by the paired
runner. Timing does not overlap builds, tests or profiling. No affinity,
frequency or desktop isolation is applied. Host: Apple M2 Max, macOS 27.2,
deployment target 26; this is not physical macOS 26 performance qualification.

## Evidence

- [Ordinary baseline](runtime-workloads-unbound-baseline-2026-10-26.json),
  [ordinary candidate](runtime-workloads-unbound-settled-2026-10-26.json),
  [ordinary repeat](runtime-workloads-unbound-repeat-2026-10-26.json),
  [original cancellable contract](runtime-workloads-unbound-cancellable-2026-10-26.json)
  and [independent fair audit](unbound-entry-fair-audit-2026-10-26.json).
- [Nine paired blocks](unbound-entry-paired-2026-10-26.json) and
  [paired audit](unbound-entry-paired-audit-2026-10-26.json).
- [Production recursion](production-cancellation-unbound-2026-10-26.json) and
  [independent recursion audit](unbound-entry-recursion-audit-2026-10-26.json).
- [Rejected initial ordering](runtime-workloads-unbound-2026-10-26.json),
  [source/compiler provenance](unbound-entry-rejected-order-2026-10-26.json) and
  [initial audit](unbound-entry-initial-audit-2026-10-26.json). Historical artifact
  hashes were audited before rebuilding the compiler; the final accepted
  artifact hashes were audited separately.

- [Qualification manifest](unbound-entry-qualification-2026-10-26.json),
  [CPU gates](unbound-entry-cpu-2026-10-26.log),
  [physical Metal gates](unbound-entry-metal-2026-10-26.log) and
  [detailed focused qualification](unbound-entry-focused-details-2026-10-26.log).
