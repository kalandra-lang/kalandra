# Iteration and independently owned generator frames

Iteration separates a reusable source from the state of a traversal. Creating a
cursor does not traverse the source. Advancing a cursor performs work until the
next value, exhaustion, or an expected error. There is no scheduled task or OS
thread associated with an ordinary cursor.

`std/option` supplies the nominal generic enum `Option[T]`: `None` represents
exhaustion and `Some {value}` represents a produced value. Distinct traversals
must have independent positions; a suspended generator instance is single-pass.
Exhaustion is sticky and must not re-execute producer side effects.

## Error type and uninhabited values

The lowercase primitive `never` has no possible values. `Iterator[T, never]`
cannot produce an expected error; `Iterator[T, E]` carries the typed error of
advancing its producer. `! never` is an infallible signature, not a sentinel
error variant or runtime wrapper. Error union normalization removes `never`;
generic callback inference must preserve that bottom-type identity while
omitting impossible runtime error paths. No value or constructor can inhabit
`never`.

## Source boundaries and collection

A loop expression containing `yield` defines a generator. Nested statement loops
and conditionals yield into that generator. A separately bound loop expression
establishes its own generator boundary. Loops without `yield` retain their
statement or single-result `break value` behavior.

`array(sequence)` explicitly consumes and materializes the produced values. The
migration from eager collecting loops changes when effects/errors happen and
when captured backing can be mutated; diagnostics and all authored examples must
migrate with that change. It cannot be implemented merely by delaying an eager
array allocation until the first `next`: partial consumption must perform only
the requested work.

## State and ownership

A generator frame owns its resume position, live locals, captures, cleanup
obligations, and terminal state. The creator's return must not invalidate it.
Advancing, closing, and transferring the frame require one consumer; aliases
cannot independently advance the same state or release a live capture.

This capability is separate from general record mutation. Source `mut` permits flat numeric/bool arrays and explicit owned-capability
operations such as advancing a cursor; iteration does not make arbitrary record
fields writable. Compiler-managed cursor/frame state has
an explicit ownership and access contract.

Borrowed array backing remains read-locked while the cursor can still read it.
Exhaustion, terminal failure, scope exit and early abandonment release captures
and owned resources deterministically. Cancellation/fault precedence and task
settlement follow the existing invocation contract. Suspending or abandoning an
active structured group requires an explicit settlement path.

## Lowering and performance requirements

Keep suspended generator frames separate from scheduler task admission. Charge
persistent state to bounded invocation-owned storage and release it at close.
Existing resumable call frames provide useful control-flow machinery but are
not an independently owned generator representation by themselves.

The synchronous frame bridge has its own backing ledger. It borrows the active
caller's cancellation token only during construction or advancement and clears
that binding before becoming dormant. Its budget adapter charges retained state
through the frame resource's live owning scopes, rather than retaining a raw
creator-task budget pointer. Captured graphs can outlive their creator. Produced
values are retained into the receiver before terminal cleanup retires storage.
The bridge neither allocates nor submits a scheduler task for a cursor.

Resource retirement detaches scope members under the registry mutex, then runs
cleanup outside that mutex. A backing pin protects each detached member. Nested
scope cleanup adds members to a per-thread intrusive retirement queue, drained
iteratively without allocating a cleanup stack. This is lifetime bookkeeping;
it is not an ambient invocation, source finalizer or scheduler callback. An
in-flight foreign operation keeps its existing resource pin and stop rules.

A producer's expected error is reported once, closes its frame, and is followed
by sticky exhaustion on later advancement. Runtime faults and cancellation keep
their existing invocation precedence; an infallible expected-error channel does
not disable those checks. Reentrant or overlapping private-ABI advancement is
rejected before touching suspended locals, in addition to source ownership checks.

Specialize source, adapter and item types before lowering. Direct iteration of
arrays/ranges and statically known lazy pipelines should lower to ordinary loops
without allocating a frame per element or adding an indirect call per element.
Escaping stateful generators require explicit owned-frame lowering rather than
an accidental task/closure emulation with different cleanup behavior.

Device-compatible iteration needs a defined Metal/Vulkan/CUDA lowering or a
clear diagnostic. Live CPU cursors are not transferred to a GPU, and host I/O
never becomes a device instruction or hidden host fallback.

Qualification compares equivalent source-level work, validates values and side
effects, and retains control/paired/replication outcomes. Existing ordinary,
resumable, array and GPU programs remain part of regression acceptance.

## Adapter ownership and errors

`std/iter.map` and `filter` consume an `Iterator[T, E]` and capture their callback
by value. The returned cursor combines source errors with callback errors;
`never` erases an absent channel. Return-type inference preserves this union in
the generic implementation. Adapters do not evaluate callbacks until requested.
`take` accepts an `i64` limit; nonpositive limits never advance their input.
Closing an adapter closes its owned source recursively through the iterative
resource-retirement machinery, including cancellation and joining of suspended
children. None of these operations creates a scheduler task per cursor.

Capturing a scalar copies its binding state. Mutation within a generator cannot
reassign the creator's binding. Array capture retains shared backing with a read
loan; explicit `copy` is required for independent storage. Captured owned handles
move into the generator. A generated frame's root loop uses that existing owner
rather than adding a second scope reservation for its source. Inner lexical
cleanup boundaries remain distinct.

Immediate `array(for ... yield ...)` and `array(while ... yield ...)`
materialization uses a private collecting function instead of constructing a
cursor that cannot escape. Its captures still have private scalar bindings and
the same publication loans. Inlining this function avoids a second invocation
boundary; it does not permit mutation of already published backing. Materializing
an existing cursor still advances and closes that cursor normally.

Tensor items cross into `Option.Some` using the existing packed record-field
representation. Canonical packed backing is retained directly. Offset or strided
views are copied in logical element order, with checked dimensions, cancellation
polling and byte-preserving copies for all ten numeric types. This normalization
does not change floating-point values or copy captured arrays implicitly.

## Local producer/consumer fusion

The CPU task backend copies fully checked IR before optimizing it. Local
single-pass scalar producers and statically known pure callbacks can be pushed
into their consuming `for`, eliminating the per-item generator/iterator bridge.
This is a compiler optimization of the existing ownership and evaluation rules,
not a new source-level iterator contract. Source loan checks run first.

Construction captures are evaluated once, in source argument order, even when
the inlined body does not use an argument. Producer bounds and bodies remain
deferred. `take(0)` does not evaluate a deferred producer bound; `take(n)` stops
without requesting item `n+1`. Consumer `continue` resumes the producer after
the current yield; consumer `break` abandons it before post-yield work. Flat
array captures must name existing non-reassigned backing; temporary arrays fall
back so eliminating a cursor cannot extend their allocation lifetime.

Scalar control transfers are normalized into structured native loops with
private pending-transfer and result cells. An assignment whose RHS transfers
control publishes no placeholder value. Loop exhaustion, valued breaks,
continues in break values and nested transfers retain their distinct paths.
The resulting native loops use the existing checked arithmetic and cancellation
lowering. No runtime polling, scheduler, resource limit or fault precedence
contract is changed. Both the function result metadata and final statement must
identify the returned value, as required by native and continuation lowering.

Pure traversal regions in larger functions can be outlined into private bulk
helpers. Inputs are scalar/POD values and read-only backing; outputs are the
private scalar bindings used afterward. A fault or cancellation aborts the
containing function before publishing those outputs. Regions containing effects,
array writes or owned-resource transitions cannot use this transformation.

Each proof and rewrite is transactional and has a code-growth bound. Partial
manual advancement, unknown callbacks, managed yielded items, producer effects,
expected errors and task suspension retain the ordinary frame implementation.
If native physical-frame admission fails, the backend replans the original
checked module using budgeted continuation storage. Mixed GPU-host lowering
retains its ordinary generator frames; this pass does not fuse GPU kernels.

Qualification checks both values and reachable emitted code: selected local
traversals must actually eliminate generator/iterator ABI calls. Exact fault
locations and left-to-right argument evaluation are checked separately. Infinite
source, adapter and outlined/control traversals test cancellation and deadlines
under all four instrumentation variants, with one and four workers.

## Performance qualification and remaining specialization

Direct array/range loops keep their existing lowering. General escaping or
effectful cursors retain a producer-frame boundary on each advancement. The
following measurements record the pre-fusion implementation; final fusion
acceptance is recorded separately below rather than replacing that evidence.

The frozen Apple M2 Max/macOS 27.2 qualification compares matching compiler,
runtime and header installations, validates every invocation, and retains nine
ABBA/BAAB blocks with three timed rounds and two warmups. Ordinary resumable
calls remain within 0.6% of baseline in paired/replicated measurements. Existing
manual cursor cases range from 0.2% to 1.1% slower in the paired comparison and
0.2% to 0.8% slower in replication. Identical-build controls remain within 0.6%.
These small unfavorable differences are retained. Cursor `for` takes 6.3–16.4%
less time than equivalent manual `next`/`close` traversal.

Immediately materialized growing arrays of `i*i%97` retain the former eager
collector's performance: paired and replicated ratios range from 0.9995 to
1.0055. This does **not** establish competitive numerical throughput. At 4,096
items, the current collector takes about 1.26 ms versus 4.26 microseconds for
the C++ `vector` and Rust `Vec` references. The references grow without reserving
capacity, retain actual storage with compiler barriers, and validate the same
scalar result. Kalandra uses O2 and includes its closed invocation/ownership
lifecycle; C++/Rust use O3 and do not implement Kalandra's cancellation/resource
contract. The older Kalandra collector has essentially the same cost.

A separate filter-even-indices/map-`i*i%97`/sum comparison exposes the cost of
general lazy state on deliberately small per-item work:

| Items | Direct range loop | Source generator | `filter` then `map` |
| --- | ---: | ---: | ---: |
| 64 | about 0.8 microseconds | about 38 microseconds | about 131 microseconds |
| 4,096 | about 3 microseconds | about 2.25 ms | about 7.6 ms |

The paired and replicated results preserve this gap. It must not be described
as zero-cost iteration or extrapolated into a whole-language speed claim.
`native/benchmarks/fair/iteration.py` builds reproducible direct, source,
adapter and materialized cases using the common frozen-driver protocol. Use
its `--baseline-mode` and `--candidate-mode` options to distinguish an
abstraction comparison from a same-source before/after regression test.
