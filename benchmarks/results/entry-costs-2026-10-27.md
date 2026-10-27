# Checked entry frames and ownership experiment — 2026-10-27

This follow-up compares the compiler against frozen native commit `ac1158a7`
with the same source algorithms, reference implementations, execution contracts
and measurement protocol. It retains the scalar frame transformation and rejects
the compact allocation-header prototype. The zero-round Rust-scalar and array
parity targets remain open. Paired zero-round scalar time falls 10.0%, with a
1.7% short-case regression; other nonempty source medians stay within 0.6%.

## Accepted compiler change

[LeafEntry.h](../../native/lib/LeafEntry.h) separates a checked scalar host entry
into a call-free prefix and an internal checked worker. Only entries already
marked by the closed unbound-source proof are eligible. Task frames, cancellation
boundaries, native pure frames and functions in special sections are excluded.

The prefix permits ordinary reads, pure instructions, selected arithmetic LLVM
intrinsics and writes to private scratch storage. Before a resource operation,
foreign call, atomic/volatile access, external store or dynamic allocation it
transfers to the original implementation with the original arguments through a
same-signature `musttail` call. No externally visible prefix effect is repeated.
SROA must eliminate every local allocation and an original value-return path must
remain; otherwise the transformation is discarded.

The accepted wrapper uses no frame pointer. Its checked worker retains the
original frame pointer, admission, fault handling, invocation cleanup and unwind
behavior. This is a compiler implementation change; the public language and task
cancellation contract stay the same. It adds code for the checked worker rather
than removing checks from managed execution.

## Qualification and compiler review repairs

The first broad source qualification rejected two software-arithmetic TSan
fixtures: a new required tail transfer omitted the original Boolean return's
`zeroext` ABI attribute. LLVM IR verification alone did not catch the target
lowering failure. The repair copies return and parameter ABI attributes onto
every transfer, with explicit return-extension and parameter-attribute regression
cases. Both formerly failing fixtures compile after repair. The
[failed run and source/IR snapshot](entry-costs-pre-abi-failure-2026-10-27.json)
and [CPU log](entry-costs-pre-abi-cpu-2026-10-27.log) are retained; that interrupted
qualification is not counted as a pass.

Final review also moved host-marker assignment before CFG simplification, which
may merge/delete the originating basic block. The proof unit requires the marker
to be present before that callback. The interrupted pre-marker CPU run is
[retained](entry-costs-pre-marker-cpu-2026-10-27.log) and does not count as final
qualification.

The timings below were collected before these compiler repairs. **All 20 measured
Kalandra executables rebuilt byte-for-byte identically with the repaired
compiler** (16 ordinary/cancellable workload executables and four production
recursion executables). The original reference executables and all other
recorded artifacts are unchanged. The
[artifact-equivalence attestation](entry-costs-equivalence-2026-10-27.json)
records old/new paths, both SHA-256 hashes, final compiler/runtime hashes and
checks of every other recorded artifact. Those executable identities allow the
measurements to be retained without selecting another timing outcome.

Raw measurement metadata retains the actual superseded compiler digest; it has
not been rewritten to imply that the repaired compiler ran those commands. The
independent numerical audits completed before repair. Their historical
`--verify-artifacts` result reflects that time: the shared compiler path now
contains the repaired binary. Use the equivalence attestation for the final
executable identity, or generate fresh evidence when rerunning the benchmark.

## Isolated performance results

Nine matched ABBA/BAAB blocks against frozen `ac1158a7`, with identical ordinary
harness inputs, show **10.00% less zero-round scalar time**. Every block wins;
individual reductions range from 3.94% to 20.97%. The 16-round scalar case is
**1.66% slower** (range: 4.22% slower to 1.82% faster). Other nonempty source
medians are within 0.59% of baseline. This is a measured tradeoff, not a claim
that every call became faster.

Positive reductions below mean faster; negative values mean slower. Every block,
including losing blocks and task-scope diagnostics, remains in the raw evidence.
Task-scope rows include whole-scope startup and constant-source specialization;
they are diagnostics rather than cross-language kernel comparisons.

| Case | Rounds | Median time reduction | Worst block | Best block |
| --- | ---: | ---: | ---: | ---: |
| arrays | 0 | -0.11% | -3.94% | 1.93% |
| arrays | 16 | 0.10% | -3.12% | 2.70% |
| arrays | 64 | -0.58% | -2.70% | 0.25% |
| arrays | 256 | 0.43% | -0.18% | 3.91% |
| scalar | 0 | 10.00% | 3.94% | 20.97% |
| scalar | 16 | -1.66% | -4.22% | 1.82% |
| scalar | 256 | 0.06% | -2.04% | 0.83% |
| scalar | 4096 | 0.29% | -7.58% | 7.64% |
| task scope / direct | 0 | 0.00% | -16.46% | 13.15% |
| task scope / direct | 16 | 3.43% | -4.54% | 8.65% |
| task scope / direct | 256 | -0.07% | -2.80% | 3.86% |
| task scope / scheduled | 0 | -0.59% | -8.27% | 2.19% |
| task scope / scheduled | 16 | -2.12% | -8.44% | 0.49% |
| task scope / scheduled | 256 | 0.84% | -4.05% | 3.03% |
| task scope / scheduled-serial | 0 | 0.12% | -7.51% | 3.72% |
| task scope / scheduled-serial | 16 | 0.44% | -1.53% | 3.38% |
| task scope / scheduled-serial | 256 | -0.10% | -1.35% | 1.03% |

The unchanged fair protocol uses two warmups, seven measured samples, three fresh
processes per cell and the same iteration count for every language in a cell.
The isolated paired study retains its existing two warmups, three measured
samples, ABBA/BAAB ordering and shared 20,000-iteration cap. All final runs request
100 ms calibration; the cap can make short cells substantially shorter. These
nanosecond observations should not be treated as universal hardware constants.

First ordinary full run (ns/invocation):

| Case | Rounds | Kalandra | C++ | Rust |
| --- | ---: | ---: | ---: | ---: |
| arrays | 0 | 42.602 | 22.640 | 22.381 |
| arrays | 16 | 345.352 | 431.283 | 442.548 |
| arrays | 64 | 1064.952 | 1953.898 | 1905.621 |
| arrays | 256 | 3897.685 | 7697.010 | 7405.300 |
| scalar | 0 | 2.650 | 4.402 | 1.419 |
| scalar | 16 | 21.442 | 27.317 | 29.029 |
| scalar | 256 | 894.171 | 937.235 | 892.606 |
| scalar | 4096 | 14809.082 | 15748.503 | 14768.268 |

Ordinary scalar zero-round times are 2.650 ns and 2.642 ns in the independent
repeat; zero-round arrays are 42.602 ns and 43.473 ns. The worst nonempty gap to
the faster C++/Rust reference is 0.28% in the first ordinary run, 7.14% in the
repeat and 4.59% under cancellable embedding. All remain within the 10% minimum
acceptance threshold on this host; they do not establish that Kalandra is always
faster. The fully cancellable harness still observes its execution contract.

The separate production recursion refresh checks the normal compiler, including
non-tail depths 100, 1,000 and 10,000. Its largest nonempty hybrid gap is 5.84%
against the faster reference, at depth 100. All 300 verification workloads and
1,120 timed observations pass the independent numerical audit.

**The zero-round target remains unmet.** In the first full run, scalar entry is
86.8% slower than Rust and arrays are 90.3% slower than the faster reference.
The remaining scalar cost includes the checked public entry/context ABI. Further
removal needs a dedicated ordinary-entry contract/ABI while preserving fault
reporting and the managed embedding path; the current pass does not introduce it.

Evidence: [ordinary](entry-costs-ordinary-2026-10-27.json),
[repeat](entry-costs-repeat-2026-10-27.json),
[cancellable](entry-costs-cancellable-2026-10-27.json),
[paired](entry-costs-paired-2026-10-27.json),
[full-run audit](entry-costs-audit-2026-10-27.json),
[paired audit](entry-costs-paired-audit-2026-10-27.json),
[production recursion](entry-costs-production-2026-10-27.json),
[recursion audit](entry-costs-production-audit-2026-10-27.json).
The earlier [frame-only exploratory run](entry-costs-frame-ordinary-2026-10-27.json)
and [audit](entry-costs-frame-audit-2026-10-27.json) precede the final protected-code
exclusion guard and are retained as exploratory evidence.

## Rejected compact backing experiment

An experimental runtime layout reduced the M2 backing header from 104 to 64 bytes,
using extended inline metadata for environment/recursive graphs. Logical quota
charges, allocation attempts, references and observers were preserved. Its
focused correctness gates passed, but performance did not justify keeping it.

Nine matched ABBA/BAAB blocks show median time changes of **0.37% slower** for
zero-round arrays and **1.73%, 3.37% and 3.26% slower** at 16, 64 and 256 rounds.
Every nonempty array block lost. The production runtime files are restored exactly
to the baseline; this layout does not ship.

The [combined run](entry-costs-combined-ordinary-2026-10-27.json),
[paired observations](entry-costs-combined-paired-2026-10-27.json),
[independent audit](entry-costs-combined-audit-2026-10-27.json) and
[rejected source snapshot](entry-costs-compact-rejected-2026-10-27.json) retain
this unsuccessful experiment. Its scalar results are preliminary combined-candidate
observations, not the final isolated result.

## Profiling limitations

The first [sampling attempt](entry-costs-profile-2026-10-27.json) caught loader
startup and cannot attribute steady-state kernel cost. The corrected
[ready-marker profiles](entry-costs-ready-profile-2026-10-27.json) wait until a
[diagnostic copy of the driver](entry-costs-profile-driver-2026-10-27.json) has
started; its only change is one ready-marker write before the batches. They show allocation/free work in the
small-array case. Sampling counts are observations, not exact exclusive time
percentages; instrumented times are not acceptance timings.

Scalar frame splitting does not solve array allocation and ownership costs.
Further array work needs an escape-aware allocation/lifetime design with the same
fault and cancellation cleanup guarantees, rather than assuming smaller metadata
will be faster.

## Reproduction

Build the native compiler and optimized task runtime with the normal CMake
configuration. Build a distinct ordinary and cancellable workload directory with
`native/benchmarks/fair/runtime_workloads.py --build-only`, then measure with
`--reuse-build --target-ms 100` and the corresponding `--execution-contract`.
Use a separately frozen compiler/runtime and identical ordinary driver inputs
for `runtime_workloads_paired.py --blocks 9 --target-ms 100`.

Run `native/experiments/cancellation/production_entry.py OUTPUT --target-ms 100`
for the production recursion suite. The numerical audit entry points are
`runtime_workloads_entry_audit.py`, `runtime_workloads_paired_audit.py` and
`native/experiments/cancellation/audit_production.py`. Keep build/test/profiling
work outside acceptance timing. A first runner attempt failed to save evidence
because the sandbox blocked CPU-brand metadata; accepted evidence was saved by
rerunning with access to that local metadata.

This study uses an Apple M2 Max on macOS 27.2, with a macOS 26 deployment target.
It does not physically qualify performance on macOS 26 or establish performance
on Linux/Windows hardware.

## Final qualification

All **420 selected CPU gates** and **50 selected Metal execution/device-codegen gates** pass
against the repaired compiler. The source-helper corpus checks 192,560 exact
numeric/first-fault cases across Debug, optimized, ASan/UBSan and TSan builds,
plus cancellation/deadline, divergent work, budgets, ownership, poisoned storage,
zero signs, subnormals, infinities and NaN payloads. The new LLVM unit covers ten
frame/effect/protected-code/ABI cases and idempotence. The separate focused rerun
includes both previously failing software-arithmetic sanitizer fixtures.

[Qualification manifest](entry-costs-qualification-2026-10-27.json),
[CPU log](entry-costs-cpu-2026-10-27.log), [CPU test output](entry-costs-cpu-2026-10-27.xml),
[Metal log](entry-costs-metal-2026-10-27.log), [Metal test output](entry-costs-metal-2026-10-27.xml),
[focused log](entry-costs-final-focused-2026-10-27.log) and
[focused details](entry-costs-final-focused-details-2026-10-27.log) retain the results.

The selected CPU suite has the prior 419 gates plus the new leaf-entry proof gate.
The Metal CTest invocation uses a separate top-level logging directory with the
original generated test definitions and working directories; selected test names
were checked against the same 50-test list before execution. These correctness
runs occur after acceptance timing and do not contribute performance samples.
