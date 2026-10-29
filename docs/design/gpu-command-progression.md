# Prepared GPU command progression

A prepared float32 region can encode its bounded chunks before its first GPU
submission. Each Metal completion callback checks device status, arithmetic
flags and its external cancellation capability, then commits the next encoded
chunk. Only the final acknowledgement wakes the source task. The callback does
not execute source code.

```mermaid
sequenceDiagram
    participant T as Source task
    participant G as Metal queue
    participant C as Completion callback
    T->>T: Validate shapes, admit workspace, encode chunks
    T->>G: Reserve external lease; commit first chunk
    loop Bounded chunks
        G->>C: GPU acknowledgement
        C->>C: Check status, flags, cancellation
        C->>G: Commit next chunk if permitted
    end
    C->>T: Release external lease
```

## Admission and fallback

The initial implementation admits prepared, nonempty, straight-line float32
regions with at least five compute instructions. It supports native matrix
products, compiler-proven non-faulting maps, transpose, softmax and RMSNorm with
host-bound valid epsilon. Shape guards and aliases are checked before encoding.
Branching, downloads, scalar results, device scalar operands, checked conversions,
other numerical formats and invalid or unproved regions retain the existing
streaming path. This changes scheduling and ownership; generated numerical
shader code is unchanged. The later [work-bounded progression](gpu-work-bounded-progression.md)
adds optional scheduling metadata to the private instruction structure.

The preflight computes shapes and conservatively sums every output, transform
scratch allocation and parameter buffer without assuming backing reuse or fusion.
It also reserves argument workspace and leaves the existing 64 KiB headroom.
If this cannot fit, execution streams through the existing chunks. Concurrent
allocations still use the ordinary atomic byte-budget admission.

Metal queues have finite pools of non-completed command buffers. Pre-encoding
must reserve that capacity before creating commands. The runtime reserves one
base slot per active frame and at most `maxInFlight` extra slots across all
prepared chains. Each queue has capacity `2 * maxInFlight`. A region reserves
an upper bound of one command per compute instruction, less its base slot. Pool
contention or a region larger than the extra pool selects streaming execution.
Reservations return after unsubmitted command objects have been released.
There is no source graph execution limit: the long-graph test executes 513 maps
in 129 chunks through the fallback.

## Cancellation, errors and lifetime

Unproved plans and streaming execution retain the original four-logical-stage /
64 Mi-work quantum. Compiler-costed hardware float32 plans now use the
[work-bounded policy](gpu-work-bounded-progression.md), allowing more than four
cheap stages in one command. An oversized kernel remains a singleton quantum,
including its previously supported fused post-map. Fusion charges both logical
instructions. These are structural work estimates, not wall-clock deadlines.

`ExternalCompletion::cancelled()` is a synchronized, generation- and epoch-checked
read of the leased task's cancellation state. Expired or stale capabilities stop
progression. Its successful read is the next-chunk submission decision; a
cancellation recorded before that decision wins. Cancellation after it can have
at most that already-admitted chunk outstanding, as with the original external
submission reservation.

One external lease pins the frame across the chain. Every referenced tensor and
pipeline remains owned by its prepared plan or invocation value table. A retained
owner table covers parameters, indirect resources and transform scratch. This
also permits unretained Metal command references within the admitted chain.
All owners and budget reservations survive until submitted GPU access ends;
unsubmitted chunks are discarded on cancellation or failure. Core destruction
still waits for that acknowledgement.

Device completion failure stops progression and takes precedence over flags and
later deferred host errors. Flags are checked before admitting another chunk and
again at final acknowledgement. Invalid metadata and unproved operations use the
original path, preserving their existing error order. Allocation failure during
pre-encoding flushes prior valid encoded work before reporting the deferred error.

## Rejected shared-event alternative

The experiment in `native/benchmarks/event_progression.py` reconstructs commit
`090d4087`, applies the archived prototype patch in an isolated directory and
compiles its runtime. It encodes one command with shared-event checkpoints and
indirect dispatch arguments. At each checkpoint the CPU checks cancellation and
fault flags, zeros later grids if stopped, then signals GPU permission. CPU
writes happen while subsequent GPU reads wait on the event.

This follows Apple's [CPU/GPU shared-event synchronization protocol](https://developer.apple.com/documentation/metal/synchronizing-events-between-a-gpu-and-the-cpu).
It does not assume that a CPU C++ atomic and a Metal shader atomic provide a
portable concurrent cross-processor cancellation word. A separate Metal witness
verifies that zero indirect grids suppress later dispatches under validation.

The event prototype is performance evidence, not production code. Full failure
and cancellation qualification is intentionally not claimed for the rejected
implementation. Its final timing evidence and the accepted-path qualification
are linked from the performance report.
