# Invocation lifecycle and private ABI qualification

Fresh invocations now initialize a small live prefix and materialize control/resource state only when needed. Immutable process fault-injection configuration is read once rather than rescanned on every first allocation. No context reuse, cancellation opt-out or fair-driver change is used.

Nine matched blocks show median elapsed-time reductions of **61.54% for zero-round arrays** and **28.77% for zero-round scalar closures** against frozen native commit `8c86ce28`. The long array case is back at parity (0.76% less time, with one small losing block) after rejecting an intermediate 4.47% regression.

**The zero-round 10% C++/Rust target remains unmet.** These are substantial overhead reductions, not universal performance parity. Both full fair runs put all six nonempty cases within 4.04% of C++ and Rust; short nonempty cases and larger arrays beat both references. Long scalar differences remain near noise.

## Cross-language results

Per-invocation nanoseconds from the first 50 ms calibration run. Ratios below one mean less elapsed time. The repeat column is the worse ratio against either reference in the independent second run.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust | Repeat worst |
|---|---:|---:|---:|---:|---:|---:|---:|
| Immutable array | 0 | 44.50 | 22.38 | 22.53 | 1.989 | 1.975 | 1.970 |
| Immutable array | 16 | 364.67 | 433.19 | 398.96 | 0.842 | 0.914 | 0.832 |
| Immutable array | 64 | 1121.41 | 1881.21 | 1744.03 | 0.596 | 0.643 | 0.636 |
| Immutable array | 256 | 4079.35 | 7706.61 | 7254.77 | 0.529 | 0.562 | 0.564 |
| Captured closure | 0 | 10.12 | 4.06 | 1.36 | 2.493 | 7.448 | 6.578 |
| Captured closure | 16 | 23.19 | 28.40 | 28.87 | 0.817 | 0.803 | 0.866 |
| Captured closure | 256 | 898.46 | 893.27 | 895.73 | 1.006 | 1.003 | 0.997 |
| Captured closure | 4096 | 14935.18 | 14883.33 | 14890.50 | 1.003 | 1.003 | 1.040 |

Zero rounds retain fresh invocation, token binding, native capability, marshalling, status/depth checks and settlement. The array also constructs its initial tensor. Ordinary internal source calls do not construct a fresh invocation per call.

## Matched before/after

Nine adjacent ABBA/BAAB blocks retain identical iterations per pair, two warmups and three measured rounds per process. Every call checks the independent exact integer oracle.

| Program | Rounds | Median time reduction | Losing/winning block range |
|---|---:|---:|---:|
| arrays | 0 | 61.54% | 59.26% to 63.90% |
| arrays | 16 | 16.25% | 14.86% to 17.25% |
| arrays | 64 | 5.17% | 4.52% to 5.84% |
| arrays | 256 | 0.76% | -0.17% to 1.51% |
| closure | 0 | 28.77% | 25.51% to 34.41% |
| closure | 16 | 10.54% | 8.76% to 15.43% |
| closure | 256 | 1.58% | 0.49% to 8.75% |
| closure | 4096 | 0.70% | -2.53% to 8.00% |

The production cancellation refresh retains 300 Fraction-oracle verification workloads and 1,120 timed samples. In the 100 ms calibration refresh all 30 nonempty C++/Rust comparisons remain within 5.37%, including non-tail depths 100/1000/10000. These are scoped host measurements. The earlier 20 ms refresh missed at depth 100 by 12.3% against C++; that result is retained. A separate quiet 13-block, 100,000-call study puts the same production case 4.9% behind C++ and 5.3% behind Rust. Every paired block in that follow-up remains within 10%, with bootstrap median-ratio intervals also below 10%. This timing sensitivity is why neither the earlier miss nor losing blocks are discarded.

Isolated redundant-admission, guard-removal and conservative stack-reservation variants produce no material gain in that follow-up. They are rejected: production retains its checked stack guards. The unchecked guard-removal probe is diagnostic only and is not performance acceptance evidence.

Complete task-scope diagnostics retain pool/monitor construction, spawn, await and destruction. Eight-worker medians are roughly flat with losing blocks; serial-scope medians improve but also retain losing blocks. Fixed-input direct diagnostics can constant-fold and are not cross-language rankings.

## Architectural change

- The cold live prefix contains the cancellation pointer, control tag/poll mode and atomic live diagnostic. Fault/depth/stack fields and the inline ledger have separate initialized-state tags. No additional heap object is introduced.
- Fault recording materializes control before reading it. Later ledger construction preserves the first fault. Both default and explicit value initialization leave deferred storage untouched.
- Only public resource admission materializes a ledger. Private reservation/ownership helpers operate on already-admitted state, avoiding redundant branches in long allocation loops.
- `KALANDRA_TEST_FAIL_ALLOC_AT` is a process default read once on first resource use. Counters remain per invocation; explicit overrides, including zero, retain precedence. Later environment mutation is unsupported; in-process tests use `failAllocationAt()`.
- Generated healthy armed-mode helper checks use acquire token loads, including null safety. Restoring an armed boundary after explicit token unbinding normalizes to unbound mode, with immediate follow-up source-entry regression checks. Faults, settlement and observers retain the observed path. Bounded entry still checks before and after admitted work.
- The private field layout changed. Rebuild generated modules and C++ header consumers together. A retained `kalandra_invocation_layout_v2` relocation rejects new modules linked to old runtimes without adding an entry call. This does not promise compatibility for older modules or a stable public C++ ABI.

## Validation

The full native build, all 419 distinct selected CPU gates and all 50 selected Metal device/codegen gates pass. The final 23 focused gates include: 12 native cancellation stress variants, eight lifecycle/environment variants, bounded-entry/local-storage proofs and the source suite. The source suite checks 192,560 exact integer values/faults across debug, optimized, optimized ASan/UBSan and optimized TSan builds from poisoned storage; array ownership, aliases, budgets, cancellation and recovery remain covered. The live diagnostic is also sampled concurrently during first materialization.

Independent audits recompute modular oracles and statistics and verify measured artifact hashes. The recursion audit uses independent polynomial/residue closed forms, a periodic Euclid identity and exact Fraction chains, cross-checked against 108 small recursive cases. Timing does not overlap builds, tests or profiling. There is no affinity, frequency or desktop isolation. Host: Apple M2 Max, macOS 27.2, deployment target 26; physical macOS 26 performance is not qualified.

## Evidence

- [Final full comparison](runtime-workloads-invocation-settled-2026-10-26.json) and [confirmation](runtime-workloads-invocation-settled-repeat-2026-10-26.json).
- [Final paired blocks](invocation-lifecycle-settled-paired-2026-10-26.json), [audit](invocation-lifecycle-settled-audit-2026-10-26.json), and [confirmation audit](invocation-lifecycle-settled-repeat-audit-2026-10-26.json).
- [Long-calibrated production recursion](production-cancellation-invocation-long-2026-10-26.json) and [independent audit](invocation-lifecycle-long-recursion-audit-2026-10-26.json). The [earlier 20 ms refresh](production-cancellation-invocation-settled-2026-10-26.json) retains its depth-100 miss.
- [Quiet depth-100 follow-up and rejected prototypes](invocation-chain-admission-followup-2026-10-26.json) with [artifact/statistical audit](invocation-chain-followup-audit-2026-10-26.json).
- [Earlier diagnostic ablation](invocation-entry-ablation-2026-10-26.json) isolates C++ callback entry/poll/allocation/checkpoint costs; it is not a language comparison and does not subtract costs from scores.
- [Rejected intermediate paired study](invocation-lifecycle-paired-2026-10-26.json) retains the long-array regression and all losing blocks.
- [Qualification and source/evidence hashes](invocation-lifecycle-qualification-2026-10-26.json).

Residual zero-round cost includes checked entry dispatch, token checks and (for arrays) ownership/allocation/alignment bookkeeping. This pass removes avoidable initialization/configuration work while retaining those guarantees. Closing the remaining gap needs further compiler/runtime work; these results do not establish universal superiority over C++ or Rust.
