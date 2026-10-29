# Work-bounded GPU command progression

Small resident transformer graphs spent most of their time submitting and
acknowledging commands. The four-stage cap split a 16-stage graph into four
commands even when its work was far below the existing 64 Mi-unit budget.
The runtime now allows compiler-costed hardware float32 plans to fill that
budget. It still submits subsequent commands only after completion, error and
cancellation checks.

## Compiler metadata

The private `NumericInstruction::workPerElement` field defaults to zero. Old
source aggregates and callers without a proof keep the four-stage policy.
This is an internal compiler/runtime contract, not a stable binary ABI.

| Operation | Admitted proof and scheduling cost |
| --- | --- |
| Elementwise map | Hardware arithmetic, no checked faults, entirely float32, finite whitelisted expression DAG. Cost is 32 × distinct expression nodes × output cells. |
| Matrix product | Hardware float32 profile. Cost is 2 × output cells × contraction extent. |
| Transpose, softmax, RMSNorm | Existing structural float32 transform profile. Cost is 8 × output cells. |
| Fused matrix epilogue | Sum of the matrix and map costs; the map is not counted as an extra dispatch. |

The map whitelist contains numeric reads, float32 literals, and scalar/tensor
unary and binary arithmetic. The shader emitter memoizes expression IDs;
uniform nodes are conservatively charged for every output cell. Conversions,
software arithmetic, calls, comparisons and unknown expression forms receive no
map proof. Existing admission additionally excludes checked maps, branches,
scalar buffers and unsupported formats. One missing or invalid proof keeps the
whole admitted plan on its previous four-stage policy.

These numbers estimate scalar work. They do not predict GPU instructions,
cycles, cache behavior, or a maximum number of milliseconds. The proof establishes
a finite supported expression structure, not a wall-clock cancellation SLA.

## Batching and cancellation

The four parameter slots now form an ownership window. A costed plan transfers
full windows into its preallocated owner vector without submitting or clearing
the accumulated work. A matrix may rotate that window early to leave room for
its fused epilogue. Parameters, scratch, tensors and pipelines remain owned until
GPU acknowledgement. Checkpoint flag indices refer to the cumulative owner
vector, including windows retained within the same command.

Before encoding the next dispatch, the runtime checks the pending command's
accumulated work. If adding the dispatch would exceed 64 Mi units, it closes the
command first. Multiplication saturates safely above the limit. As before, an
individual kernel that already exceeds the budget runs as a singleton quantum;
this change does not make a running Metal kernel preemptible.

Cancellation can therefore cover more than four cheap logical stages before
its next checkpoint. The earlier stage-count bound is deliberately replaced
for these proven plans; there is no claim of unchanged cancellation latency.
Cancellation recorded before the next-chunk decision suppresses that chunk.
A cancellation racing that decision can leave one admitted command outstanding.
The final acknowledgement, cancellation/fault precedence and cleanup contract
remain unchanged.

Pre-encoding still requires byte, argument and command-pool reservations.
The existing conservative `computes - 1` command-credit reservation remains;
large plans or resource contention use streaming. This is admission to an
optimization, not a graph execution limit. The 513-stage test still completes
through the streaming fallback. Low-memory and unproved paths retain four-stage
chunks. `Options::workBoundedProgression = false` is a private benchmark control.

## Verification

Prepared-program tests cover small and large transformer graphs, exact agreement
with streaming, changing captures, concurrent invocations, allocation failures,
held completion cancellation/deadlines/device faults at every command boundary,
and teardown. Rank 1/3/8 chains cover 5, 9, 65 and 513 stages; zero, invalid,
partially missing, ordinary, expensive and saturating costs test chunk boundaries
and fallback. Compiler tests ensure that cost grows with expression size and
software arithmetic never receives this proof. Existing checked-conversion tests
preserve fault precedence.

The acceptance driver compares the frozen prior runtime, the new runtime, and a
same-binary control with work batching disabled. It reuses exactly the same Metal
libraries, checks every result cell against an independent oracle, verifies
unchanged CPU LLVM and shader text, and uses separate fresh-process ABBA/BAAB
measurements without validation or profiling enabled. MPSGraph remains a
benchmark reference only.
