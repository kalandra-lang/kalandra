# Native task helpers, closures and array lifetimes

The CPU task planner now sends ordinary synchronous call graphs through the
native compiler. Tasks still own their outer scheduling frame and result
transport, but a pure recursive helper no longer builds a continuation frame at
every source call. Expected-error and coordinating callables retain their
existing checked representation. Native admission failure retries that
representation; the optimization does not impose a new source restriction.

Pure native work occupies its worker until return, fault or cancellation; source
calls inside that kernel are not implicit scheduler yields. Await/join and
coordinating helpers retain their suspension points. The bounded pool is not a
preemptive executor for pure CPU work.

After full graph inlining, a conservative escape proof promotes small local
closure environments to stack storage and scalar replacement. Escapes, foreign
calls, pointer identity, uncertain uses and allocations on CFG cycles stay
managed. Eligible rank-one numeric self-tail recursion becomes a structured
loop, allowing dead intermediate arrays to be released between iterations.
Read-only tensor parameters preserve immutable inputs and avoid defensive caller
copies. The allocator retains one exclusive, fully released numeric buffer of at
most 256 bytes for exact-size reuse, including its still-live header.

These changes preserve checked arithmetic, evaluation order, strict floating
point, immutable aliases, cancellation boundaries, retained readers and logical
allocation failures. Reused buffers still consume the existing cumulative task
byte quota until settlement. Constant live memory does not mean unlimited work
under a fixed allocation budget. No source algorithm substitution, unchecked
mode, numerical-library substitution or weakened reference implementation was
used.

## Measurement contract

Measurements use the M2 Max host, the production Darwin ARM64 hybrid cancellation
entry and LLVM 23.1.3. Kalandra includes a fresh invocation, checked arithmetic,
automatic cancellation and cleanup per call. C++ and Rust retain their ordinary
semantics and may specialize their templates/generics normally. Every language
uses the same external C++ harness, runtime inputs and equivalent source-level
recurrences. Each immutable array pass produces a fresh logical output. Earlier
release of an unobserved backing is a compiler optimization.

The full cross-language run has 99 measured processes plus 99 separate edge
verification workloads. Two baseline/candidate experiments use adjacent
ABBA/BAAB processes, equal iteration counts, two warmups and three retained
rounds per process: seven blocks at 20 ms and nine blocks at 50 ms. Exact integer
oracles check every call, including warmups. A separate audit uses modular
exponentiation rather than the benchmark's iterative oracle. Profiling runs
separately from acceptance timing. There is no CPU affinity, frequency or desktop
isolation; these results establish neither general language parity nor
Linux/Windows performance.

Zero-round controls expose fixed entry costs and are not useful-work parity
claims. The task diagnostic includes fresh scope, worker/monitor lifecycle,
spawn, await and destruction. It measures complete scopes, not steady-state
queue latency, and does not rank other languages' schedulers.

## Final results

Two complete fair runs are retained. Ranges below are the observed range across
those runs, not confidence intervals. Times include the checked entry.

| Workload | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust |
| --- | ---: | ---: | ---: | ---: | ---: |
| Array 0 | 120.42–121.20 | 22.41–22.68 | 21.78–22.31 | 5.34–5.37 | 5.43–5.53 |
| Array 16 | 665.82–683.66 | 432.50–440.08 | 405.05–424.57 | 1.54–1.55 | 1.61–1.64 |
| Array 64 | 2091.74–2182.23 | 1850.64–1893.63 | 1705.15–1772.47 | 1.13–1.15 | 1.23–1.23 |
| Array 256 | 7823.99–7986.11 | 7551.36–7875.42 | 6987.25–7457.56 | 1.01–1.04 | 1.07–1.12 |
| Closure 0 | 16.99–17.35 | 4.06–4.47 | 1.35–1.52 | 3.88–4.18 | 11.39–12.55 |
| Closure 16 | 48.35–52.51 | 27.31–28.92 | 26.74–28.47 | 1.77–1.82 | 1.81–1.84 |
| Closure 256 | 1002.00–1084.19 | 896.99–959.16 | 889.67–924.12 | 1.12–1.13 | 1.13–1.17 |
| Closure 4096 | 16391.44–17390.96 | 14878.87–15382.11 | 15044.37–15621.11 | 1.10–1.13 | 1.09–1.11 |

The 256-step array case is within 1–4% of C++ and 7–12% of Rust.
This is closer to the target, not a consistent 10% result against both. Short
arrays and closures remain substantially behind. The 4096-step closure is
roughly 10–13% behind C++ and 9–11% behind Rust in these two runs.

Baseline is the frozen `f076028b9a4ada50c8bfff42353a0ab012f56ddd` compiler and runtime.
Positive values below mean less time than baseline; negative values mean a
regression. The last column retains the extrema of all paired blocks.

| Workload | 7-block median reduction | 9-block median reduction | All-block range |
| --- | ---: | ---: | ---: |
| arrays 0 | 0.37% | -0.85% | -5.53–6.45% |
| arrays 16 | -0.70% | -0.06% | -4.09–2.94% |
| arrays 64 | 16.39% | 16.65% | -18.44–21.02% |
| arrays 256 | 22.42% | 22.17% | 20.30–23.92% |
| closure 0 | 85.55% | 85.60% | 85.03–88.18% |
| closure 16 | 71.42% | 71.25% | 70.75–72.78% |
| closure 256 | 10.37% | 10.67% | 7.30–12.87% |
| closure 4096 | 0.39% | 0.71% | -1.63–1.37% |

The 16-step array case is approximately flat, including small negative paired
medians. One 64-step block is substantially slower; the confirmation retains
positive reductions in every block. Long closures are roughly flat versus
baseline, despite the large improvement in short closure setup.

The following complete 256-step scope timings use the longer paired run.
The eight-worker scope contains one child and includes pool startup/teardown.

| Entry | Baseline µs | Current µs | Paired median reduction |
| --- | ---: | ---: | ---: |
| Synchronous fixed entry | 1.227 | 1.092 | 10.54% |
| Scope, 1 worker | 57.295 | 27.255 | 52.99% |
| Scope, 8 workers | 1064.687 | 236.167 | 78.38% |

The independent production recursion refresh checks 300 edge workloads and
retains 1120 timed samples. All 30 non-empty hybrid/reference comparisons remain
within 10%; the worst ratio is about 1.066. At non-tail depths 100/1000/10000,
C++ gaps are approximately 6.5%/6.2%/3.4%, and Rust gaps are 6.5%/4.9%/4.5%.
Zero-depth controls are retained in the raw record.

Raw evidence: [fair run](runtime-workloads-final-native-2026-10-26.json),
[fair repeat](runtime-workloads-final-repeat-2026-10-26.json),
[7-block pairs](task-native-final-paired-2026-10-26.json),
[9-block confirmation](task-native-final-confirmation-2026-10-26.json),
[recursion](production-cancellation-final-native-2026-10-26.json),
[final profile](task-native-final-profile-2026-10-26.json),
[paired audit](task-native-final-paired-audit-2026-10-26.json),
[full evidence audit](task-native-final-evidence-audit-2026-10-26.json).
The final audits recompute observations/statistics and verify the measured
artifact hashes against the retained binaries before qualification.

## Experiments and remaining cost

All intermediate timing records remain available. Native task calls and local
closure promotion helped immediately, while arrays initially stayed flat:
recursive unwinding retained every intermediate array, preventing useful buffer
reuse. The first structured-loop representation bounded memory but made
bufferization copy loop state unnecessarily. Splitting terminal work from the
recursive arm, bypassing a simple initial base case and declaring read-only
inputs reduced that overhead. Reusing an exclusive cached header is the final
allocator experiment; its result is reported separately from the earlier loop
candidate.

The [pre-header-reuse profile](task-native-profile-2026-10-26.json) shows that
allocation bookkeeping and helper status checks remain significant in the array
path, while actual system allocation is rare. Sample counts are diagnostic
observations, not precise exclusive-time percentages. Captured scalar arithmetic
and fixed checked-entry cost remain relevant for short closure workloads.

The final closure IR also retains a widened checked addition in the steady-state
loop. The first iteration establishes bounds on the seed, but those constraints
are not fully propagated through the captured expression and loop backedge.
Further range analysis is a concrete next target; no overflow guard was removed
on the strength of an unproved benchmark-input assumption.

## Correctness

The new `native-task-native-helpers` gate exercises exact task results with
1/2/4 workers, scalar closures, simultaneous array argument replacement, aliased
inputs and changing-length views. It checks million-step closures under a small
task budget, every array allocation-failure point, accepted/rejected task byte
budgets, immutable inputs, cancellation/deadlines and settled ownership.

Additional harnesses check a constant live-backing bound through 100,000 array
steps, divergence until cancellation, actual native cancellation redirects when
supported and all ten numeric types. Binary32/binary64 unary maps are checked
bit-for-bit against signed zeros, subnormals, infinities and NaN payloads.
Hardware arithmetic runs in debug, optimized, ASan/UBSan and TSan builds;
optimized software floating arithmetic is checked separately. A large
source-valid helper verifies fallback to checked continuation storage.

Allocator tests cover repeated reuse, retained and transported readers,
concurrent final-reader release, numeric-to-record reuse, failure injection and
cumulative quota accounting. LLVM-level local-storage tests reject escaping,
cyclic, oversized, identity-sensitive and task-frame allocations.


## Final qualification and retained experiments

The final full build succeeds. All 380 selected CPU gates and 50 actual Metal
checks pass on this Mac, covering debug, optimized, ASan/UBSan and TSan variants
where applicable. The helper gate includes four Boolean-result programs in
addition to the numeric cases above. The earlier six focused allocator/helper
checks also pass. Test selections and complete output are retained:
[CPU selection](task-native-cpu-selection-2026-10-26.txt),
[CPU regression](task-native-cpu-regression-2026-10-26.log),
[Metal selection](task-native-gpu-selection-2026-10-26.txt),
[Metal regression](task-native-metal-regression-2026-10-26.log),
[focused checks](task-native-focused-2026-10-26.log),
[build](task-native-build-2026-10-26.log),
[repeat audit](task-native-final-repeat-audit-2026-10-26.json),
[qualification hashes](task-native-qualification-2026-10-26.json).
The live-owner instrumentation bounds invocation owners; the allocator may also
retain its one fully released cached backing until settlement.

Intermediate measurements are retained rather than substituted for final data:

- Native task calls and closure promotion, before array-loop lowering:
  [fair run](runtime-workloads-native-calls-2026-10-26.json),
  [pairs](task-native-paired-2026-10-26.json),
  [confirmation](task-native-confirmation-2026-10-26.json).
- First array-loop representation, with redundant copies and a zero-round loss:
  [fair run](runtime-workloads-tail-loops-2026-10-26.json),
  [pairs](task-native-tail-loops-paired-2026-10-26.json),
  [confirmation](task-native-tail-loops-confirmation-2026-10-26.json),
  [recursion](production-cancellation-tail-loops-2026-10-26.json).
- Split loop, simple base-case bypass and read-only inputs, before header reuse:
  [fair run](runtime-workloads-qualified-2026-10-26.json),
  [pairs](task-native-qualified-paired-2026-10-26.json),
  [confirmation](task-native-qualified-confirmation-2026-10-26.json),
  [recursion](production-cancellation-qualified-2026-10-26.json).

The [intermediate audit](task-native-intermediate-audit-2026-10-26.json)
recomputes paired statistics and exact modular oracles. It does not claim that
all historical compiler binaries remain available. Final measured artifacts and
the frozen baseline are independently hash-verified. Two broad intermediate
regression runs were stopped while the implementation was changing; only the
completed final run is claimed as qualification.
