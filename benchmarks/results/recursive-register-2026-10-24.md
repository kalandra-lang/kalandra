# Private recursive state and bounded expansion

At depth 14, the matched control reduces **unbound polynomial time by 60.4%** and **unbound branching time by 57.3%** beyond `7b51960a`. GCD is effectively unchanged. The fresh six-depth source comparison beats C++/Rust on polynomial and branches at five depths, but depth 12 still misses the target and GCD remains substantially slower. The Rust/C++ 10% target is **not complete**.

## Matched before/after control

Both frozen/current compilers lower identical source and link the same current optimized runtime archive. Clang -O2, separately rounded arithmetic, invocation lifecycle, binding and checked execution remain enabled. Each of nine adjacent ABBA/BAAB blocks per algorithm/mode uses equal iteration counts, two warmups and three measured batches in fresh processes. All warmup/measured checksums match independent integer/Fraction/GCD references. The table reports medians of per-block time reductions.

| Algorithm | Invocation | Median time reduction | Range across nine blocks |
| --- | --- | ---: | ---: |
| polynomial | unbound | 60.4% | 59.9% to 61.1% |
| polynomial | bound | -0.8% | -2.6% to 2.0% |
| polynomial | observed | 0.2% | -1.1% to 1.5% |
| branches | unbound | 57.3% | 56.0% to 59.0% |
| branches | bound | -1.7% | -3.8% to -0.5% |
| branches | observed | 0.4% | -0.9% to 3.9% |
| euclid | unbound | 0.4% | -2.2% to 5.3% |
| euclid | bound | 0.3% | -3.2% to 2.0% |
| euclid | observed | -1.4% | -2.1% to 0.0% |

Positive reductions mean less time. Bound/observed bodies retain their preceding implementation, but code layout and scheduling still affect timings: bound branches are about 1.7% slower and observed GCD about 1.4% slower in this repeat. These controls are disclosed, not counted as improvements. CPU frequency/temperature is not pinned.

[Final raw control](recursive-register-paired-2026-10-24.json). The [initial control](recursive-register-paired-initial-2026-10-24.json) independently found 60.8%/57.3% unbound reductions. The final repeat aligns saved harness hashes after restricting the optional depth argument to the drivers’ existing maximum 14. Source algorithms and numeric operations are unchanged.

## Equivalent source algorithms at six depths

The [full five-language table](fair-source-recursive-register-2026-10-24.md) measures depths 9–14, including all four phases of the private expansion. All 180 preflight reference workloads and every timed/warmup result pass. C++, Rust, Go, Python and Kalandra source algorithms and driver validation are byte-identical to the preceding comparison; only the orchestrator can now select more already-supported depths.

| Algorithm | Depth | Kalandra µs | C++ µs | Rust µs |
| --- | ---: | ---: | ---: | ---: |
| polynomial | 12 | 16.832 | 9.745 | 10.422 |
| polynomial | 14 | 28.522 | 38.836 | 41.951 |
| branches | 12 | 16.401 | 12.664 | 12.574 |
| branches | 14 | 33.269 | 50.219 | 49.977 |
| euclid | 12 | 99.624 | 31.044 | 38.804 |
| euclid | 14 | 424.540 | 125.247 | 157.721 |

Depth 12 exposes a real expansion-phase cliff: polynomial remains about 1.7× C++ and branching about 1.3×. GCD remains roughly 2.5–3.4× C++ across these depths; its helper call excludes it from private register-state specialization. The phase cliff and GCD are remaining work. No tensor intrinsic, numerical library or GPU kernel substitutes for any recursive algorithm. General GPU recursion remains unsupported.

## What the compiler proves and changes

The existing closed-scalar call-graph proof excludes memory, callbacks, ownership, suspension and token rebinding. A further proof restricts this transformation to unbound, loop-free workers whose only calls are direct self recursion, the established depth-free fault/poll helpers and memory-free intrinsics. Helper calls and tail loops retain their existing paths.

The private worker carries logical depth as an argument and returns a scalar value plus a success bit. Admission still tests the original 128-slot limit. Every terminal runtime fault is recorded and makes the local success state false; self-call success is propagated through the original failure branches. The first entry poll observes any already-faulted context. Later status polls can use the proven local state only when their uses are zero equality tests and entry dominance is established. Unbound exclusive ownership provides no asynchronous observer or token input.

A small existing-signature bridge captures/restores the published outer depth. It admits no logical slot and cannot release the outer stack lease. Public signatures, callable identities and runtime ABI are unchanged. Bound-token and observed implementations remain on their preceding paths.

The compiler then expands at most four self-call sites from one frozen private body. It preserves every copied logical admission, actual stack-pointer check, arithmetic/fault branch and result order. The snapshot never recursively grows. Only a fully successful expansion within the unchanged 16 KiB conservative budget is adopted; otherwise the original memory-based worker remains. The bridge must fit 512 bytes. Expansion occurs in stable source order for reproducibility. The native reserve remains 128 KiB.

## Qualification

- **761,424 current-side differential/reference cases** exactly match the pushed baseline on this Mac: 716,496 existing integer/tail/float32/float64 cases under both arithmetic policies, plus 44,928 new typed recursive cases for all ten numeric types and Bool. Debug, optimized, ASan/UBSan and TSan modes pass.
- All 44 typed standalone CLI executions also pass across the four build modes.
- The typed oracle independently checks overflow, separately rounded sums, signed zero, NaNs/infinities, logical depth, existing faults, immediate cancellation and observed state. Existing suites retain conversion/division faults, FMA witnesses, asynchronous token cancellation, migration and near-reserve physical stacks. Exact result bits, fault code/location, observed poll count and final depth match the baseline.
- **828 machine-frame records across 76 binaries**, including bridges and the new private value/status ABI, remain below 16 KiB; the maximum is **1,200 bytes**.
- **24 broader CTest groups pass**, including syntax debug/ASan, structural/fallback checks, recursive workers/source, parallel maps, task errors/parameters, fast calls, arithmetic policies, CPU numerical optimization and every timed matrix-cell reference. Runtime groups compile with the current compiler rather than using prebuilt old kernels.
- The structural unit checks internal signatures, retained entry polls/SP checks, helper exclusion, source identity, reversed module order and a large scalar body whose extra private expansion must fall back without changing acceptance.
- All 39 native documentation snippets across 20 pages/63 rendered blocks pass: 32 CPU programs, two physical Metal programs, four companion modules and one expected diagnostic. Workflow formatting and all 28 release/provisioning unit tests pass. No-RTTI compilation of the final implementation passes without a missing CallbackVH typeinfo reference.

[Full records and logs](recursive-register-validation-2026-10-24.json), [frame audit](recursive-register-stack-audit-2026-10-24.json), and [compiler invariants](../../docs/design/checked-call-optimization.md).

## Windows correctness fix

The preceding head’s new GCD package fixture caught a pre-existing lexer bug: Windows returned the initial dividend instead of the GCD. `take()` mutated the cursor in one `emit()` argument while another argument read its end position. Windows evaluated those arguments in the opposite order, so `==` received a one-character span and the parser selected its >= fallback. This was independent of the tail-return arithmetic change.

Commit `d5cdded6` sequences cursor updates before token emission for compound arrows and comparisons. Exact-span/AST tests cover every comparison, adjacent/spaced tokens and LF/CRLF. Full hosted Windows build/reproducibility/downloaded-package qualification passes for that isolated fix; the typed frontend diagnostic passes as well. [Captured failure and fixed-run evidence](windows-operator-span-2026-10-24.json). The temporary diagnostic workflow is removed.

Hosted Windows now gains the independent typed recursive runtime oracle. Downloaded Linux/Windows acceptance additionally checks recursive values in all numeric types and Bool. Hosted qualification for the new optimized head is pending; the preceding Windows success must not be read as qualification of this private ABI change.

## Retained experiments and next work

The [depth-only experiment](recursive-depth-only-2026-10-24.json) did not justify adoption; its [header](recursive-depth-only-2026-10-24.header.txt) and [caller context](recursive-depth-only-2026-10-24.context.txt) snapshots match its recorded hashes. Early local trials also showed that status/depth changes alone were insufficient; the qualified result is the combined, budgeted transformation above. The final raw records establish its performance, not those exploratory timings.

Next priorities are eliminating unnecessary leaf frames across expansion phases and reducing GCD’s remaining helper/checked-depth overhead through additional proofs. Safety guards and numerical semantics remain requirements, and the overall Rust/C++ target remains unmet.
