# Carry entry status through closed private recursion

Compared with pushed checkpoint `9c9b3cfe`, matched depth-14 controls measure **6.7% less polynomial time** and **4.8% less branching time**. The bridge now polls entry status once; proven closed, unbound recursive edges pass status in the private ABI. Numerical operations, source algorithms, logical-depth limits and physical-stack guards are unchanged. **The Rust/C++ 10% target is still unmet at depth 12 and for GCD.**

## Matched control

Both compilers lower identical source algorithms and link the same optimized runtime. Clang uses -O2 and -ffp-contract=off. Nine adjacent ABBA/BAAB blocks use equal iterations, two warmups and three measured batches per fresh process. Every result matches an independent integer/Fraction/GCD reference exactly. Positive time reductions mean less time.

| Algorithm | Invocation | Median time reduction | Range across nine blocks |
| --- | --- | ---: | ---: |
| polynomial | unbound | 6.7% | 4.9% to 10.5% |
| polynomial | bound | 0.2% | -2.2% to 2.2% |
| polynomial | observed | 1.3% | -2.1% to 2.8% |
| branches | unbound | 4.8% | 3.1% to 14.8% |
| branches | bound | -0.1% | -4.7% to 1.4% |
| branches | observed | 0.9% | -0.4% to 7.7% |
| euclid | unbound | -1.7% | -5.4% to 2.4% |
| euclid | bound | 0.3% | -3.9% to 4.5% |
| euclid | observed | -0.6% | -4.4% to 0.9% |

Unbound GCD is **1.7% slower** in this repeat and 2.8% slower in the initial run. It is excluded from this optimization, and all eight relevant public/private GCD/tree LLVM function bodies are byte-identical before and after; their hashes are retained in the validation record. A layout or runtime-environment effect is plausible, but its cause is not isolated. This is a recorded regression, not a claimed improvement. Bound/observed controls also vary; CPU frequency and temperature are not pinned.

[Final raw control](recursive-status-paired-2026-10-24.json) and [initial control](recursive-status-paired-initial-2026-10-24.json). The initial run measured 6.1%/5.5% less unbound polynomial/branch time. Both runs retain every sample.

## Equivalent source algorithms

The [full five-language table](fair-source-recursive-status-2026-10-24.md) covers depths 9–14. All six language/driver sources are byte-identical to the preceding comparison. The 180 preflight workloads and every warmup/measured checksum match independent references. There are no tensor built-ins, library substitutions or changed recursive algorithms. General GPU recursion remains unsupported.

| Algorithm | Depth | Kalandra µs | C++ µs | Rust µs |
| --- | ---: | ---: | ---: | ---: |
| polynomial | 12 | 17.026 | 9.953 | 10.581 |
| polynomial | 14 | 26.809 | 39.313 | 41.949 |
| branches | 12 | 16.463 | 12.860 | 12.602 |
| branches | 14 | 31.186 | 49.860 | 50.245 |
| euclid | 12 | 103.002 | 30.790 | 38.954 |
| euclid | 14 | 419.778 | 124.780 | 162.625 |

Polynomial/branches beat C++ and Rust at five of the six depths. At depth 12, polynomial is still about 1.7× C++ and branches about 1.3×. GCD remains about 2.5–3.4× C++ over the six depths. These are narrow source/runtime workloads, not a language-wide ranking. Kalandra retains checked arithmetic, invocation admission and depth/cancellation checks; the other languages retain their normal semantics.

## Why depth 12 is slow

`native/benchmarks/recursive_call_counts.py` inserts volatile counters at private worker entry points in a separate generated executable. It checks every numerical result and discards instrumented timings. The counts per original polynomial invocation are:

| Input depth | Private frame entries | Entries immediately taking the base case |
| ---: | ---: | ---: |
| 9 | 273 | 0 |
| 10 | 273 | 0 |
| 11 | 273 | 0 |
| 12 | 4,369 | 4,096 |
| 13 | 4,369 | 0 |
| 14 | 4,369 | 0 |

At depth 12, 4,096 of 4,369 private entries immediately take the base case. At depths 13 and 14, the existing frozen expansion absorbs those base cases into already allocated frames. Passing status removes redundant state reads but does not remove this frame-allocation phase cliff. A generic leaf-frame split is the next investigation; changing expansion depth to favor one benchmark would merely move the cliff. [Raw call counts and commands](recursive-status-call-counts-2026-10-24.json).

## Proof and scope

Eligibility is unchanged: only unbound, loop-free, closed scalar workers whose calls are direct self recursion, established poll/fault helpers or memory-free intrinsics. Helper-containing functions and tail loops retain their preceding implementations. The existing bridge reads entry status once and passes that exact i64 value to the private worker, which uses it in the original entry branches. Recursive edges pass the sticky local success state; explicit faults still record their original runtime code/location and clear success. Results still carry the scalar value and success bit.

Exclusive invocation ownership and the absence of a token, observer, callback or helper prevent external status changes inside this chain. Every possible internal status change is an explicit fault or a self-call result. Bound-token acquire loads and observed poll counts retain their established paths. Published depth is restored by the bridge, which owns no logical slot or outer stack lease. Public callable signatures/identity and the runtime ABI remain unchanged.

The same one-level frozen expansion and complete budget fallback remain. The conservative frame budget is 16 KiB, bridge allowance 512 bytes, native reserve 128 KiB and logical limit 128 slots. Added private arguments and outgoing copies are charged to that budget. Structural checks now require one bridge poll, no private depth/status memory reads or polls, all sixteen recursive edges, and the retained physical SP checks.

## Verification

- **761,424 current-side differential/reference cases** match the frozen pushed compiler exactly: 716,496 integer/tail/float32/float64 cases across hardware and software arithmetic, plus 44,928 cases covering all ten numeric types and Bool. Debug, optimized, ASan/UBSan and TSan builds pass.
- Result bits, overflow/conversion/division faults, fault locations, signed zero, special floating values, rounding/FMA witnesses, observed poll counts, preexisting faults and final logical depth match independent references and baseline state.
- All **44 standalone scalar executables** pass. **24 broader regression groups** pass, including asynchronous cancellation, migration, near-reserve stacks, helper/budget exclusions, source identity, worker tasks/errors/parameters, numerical optimization and benchmark-cell correctness.
- **828 audited machine-frame records across 76 binaries** remain below 16 KiB. Maximum measured conservative frame: **1,200 bytes**.
- Every runnable documentation example passes: 20 pages, 63 rendered blocks, 39 native snippets including 32 CPU programs, two physical Metal programs, four companion modules and one expected diagnostic. Windows/setup recipes are recorded separately on this Mac.
- The diagnostic call-count tool verifies numerical checksums and the six-depth frame-entry histogram independently of timed binaries.

[Validation, source hashes and logs](recursive-status-validation-2026-10-24.json), [frame audit](recursive-status-stack-audit-2026-10-24.json), and [compiler invariants](../../docs/design/checked-call-optimization.md). Linux and Windows hosted qualification passed for baseline `9c9b3cfe`; macOS release qualification was still running during this checkpoint. This change is locally qualified; its own hosted runs start after push.

Next work remains eliminating the proven leaf-frame cliff and reducing GCD helper/depth overhead without relaxing language semantics.
