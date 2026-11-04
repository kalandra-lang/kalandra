# Local mutation and read-only task sharing

Accepted direction, 2026-11-02. Implementation tracked in #266. This document
records the language rules and their implementation rationale. It supersedes the
universal-immutability requirement in the original core proposal for the local
bindings and arrays admitted below.

## Why change the model?

A private counter or array builder does not require universal immutability to be
safe. Automatic lifetime management prevents dangling storage; it does not prevent
concurrent reads and writes. We will permit local mutation and reject shared
mutation at compile time, while retaining functional construction and transforms
as first-class APIs. There are no implicit locks, ownership moves, reference
sigils, or source lifetime annotations in this direction.

## Observable behavior

- `a = 10; a = double(a)` rebinds a local name. A callee cannot change its caller's
  scalar binding. Capturing a scalar takes its value at capture time.
- `b = a` aliases array storage. An in-place edit through either name is visible
  through the other. `copy(a)` creates independent storage. Nested mutable storage
  requires an explicit copy-depth policy before it is supported.
- `sorted(a)` produces a fresh array. `sort(a)` changes the existing array.
  `generate(count, fn(index): expression)` constructs an array functionally, in
  ascending index order, without requiring an intermediate array of indices.
- Ordinary parameters and captures grant read access. Writing caller-owned
  storage requires an explicit mutating parameter contract, including in function
  types and indirect calls. The spelling is `mut`, as specified below.
- A task can mutate storage it creates privately. Parent storage passed to or
  captured by a task is read-only. `spawn sort(a)` must be rejected.
- While any unfinished task may read storage, the parent and every alias must be
  unable to mutate that storage. Multiple readers are allowed. Cancellation is
  only a request; it does not release access before execution actually settles.
- A proven settlement boundary releases that reader's access. Awaiting one of
  several readers does not release the others. Awaiting only one conditional
  branch cannot prove unconditional settlement after the join.
- Tensors retain value semantics. No new GPU shared-memory, barrier or atomic
  model is implied. User-written kernels remain a separate dependent proposal.

Illustrative pseudocode:

```text
a = [3, 1, 2]
b = a
task = spawn readAndDoSomething(a)
sort(b)                       // reject: same backing still has a task reader
await task
sort(a)                       // allowed after proven settlement

snapshot = copy(a)
task = spawn readAndDoSomething(snapshot)
sort(a)                       // allowed: independent storage
await task
```

Aliasing includes captures, returned values, record fields, views and transfers.
Transfers must not be assumed to copy on unified-memory hardware. When a proof is
unavailable, reject the write and identify the outstanding read and a remedy.
Rebinding a name does not grant write access to read-only storage.

## Implementation and qualification boundaries

1. Restore functional construction APIs independently of mutation. `generate` is
   implemented: runtime/constant sizes, empty arrays, ten numeric
   types, owned elements, callbacks/captures, worker import, allocation failure and
   cancellation are covered. Editor completion, hover and signature help use its
   checked types. `copy` is also implemented for numeric, Bool and String arrays;
   raw-bit tests check separate backing, strided inputs, independence after source
   changes, retained results and allocation-failure cleanup. Existing runtime
   quotas apply; no unchecked allocator is added.
2. Numeric/Bool rebinding and statement/expression `if`, `while` and `for` are
   implemented. Scalar types remain fixed. Statement loops use bare `break`;
   single-result expression loops require a normal-completion `else` result and use
   `break value`. `continue` advances the nearest lexical loop. Every result
   path has a compatible completed type; no last-iteration result is inferred.
   Array iteration pins one input backing; integer `range(stop)`,
   `range(start, stop)` and `range(start, stop, step)` evaluate arguments once,
   allocate no array, exclude the stop, reject zero steps with an arithmetic
   fault, and terminate safely when the final step would overflow.
   Branch and iteration declarations remain local. A for binding shadows an
   outer name without changing it. Closures capture scalar snapshots and cannot
   jump into the caller's loop. Every successful backedge, `break` and
   `continue` must discharge iteration-owned task obligations. Incoming
   obligations survive zero iterations and early exits. Exited completed
   structured groups close and release their records before transferring
   control. Cancellation requests never count as settlement.
   Loops retain cancellation/fault checks; ordinary CLI proofs may omit checks
   when the existing execution contract allows it. General Unit-returning APIs
   are outside this change.
3. Numeric and Bool arrays use an identity-preserving native representation.
   The earlier immutable MLIR tensor representation could clone
   returned arrays or reuse functional results; inserting writes into it would
   violate the intended aliases. Identity returns, fresh transforms, allocation
   ownership and CPU/task ABI boundaries require explicit regression coverage.
4. Explicit mutating contracts and the storage/read-access proof are implemented
   and locally qualified. Branches conservatively
   retain possible outstanding readers; loops account for zero iterations and
   require iteration-owned tasks to settle. Element writes and sorting use
   this same proof.
5. Bounds-checked numeric/Bool element writes are implemented and locally
   qualified, together with in-place `sort` and fresh `sorted`. Document supported element
   types, copy depth, float ordering and cancellation behavior. In-place mutation
   need not be transactional, but must remain memory-safe on every exit.
6. Update user documentation, formatter, editor, examples and migration notes;
   run numerical, ownership, cancellation and backend regression suites. Measure
   equivalent algorithms rather than substituting native library operations.

Each stage must preserve fault precedence and resource limits. No pass may hide
live array references from reclamation analysis. Read-only inputs must never gain
write permission merely by passing through an unknown function. Conditional
settlement must not use the existing union-of-observed-tasks bookkeeping as an
exclusive-access proof.

## Array ownership boundaries

Numeric and Bool arrays lower to rank-one memrefs with explicit loads and stores. Source
arrays retain their identity through synchronous calls, records, closures and
settled task results. Private array-returning implementations carry ownership
information; their C and task adapters keep the existing collection descriptor
layout. The ownership pass may return a borrowed reference without cloning its
data. Layouts remain predictable at these boundaries. Programs that lower no
numeric or Bool array references retain the existing tensor-only lowering pipeline.

`generate`, `map`, `copy`, `slice` and `concat` allocate fresh outer storage.
Array-to-tensor and tensor-to-array conversions also copy: editing an array must
not change an immutable tensor or a buffer already used by a GPU operation.
Empty arrays require no backing and use canonical empty descriptors at opaque
boundaries. No pointer-identity operation is exposed by the language.

Before a reference enters an opaque record, closure or ABI descriptor, generated
code retains an invocation owner while the memref remains visible to reclamation
analysis. Ordinary deallocation retires its ordinary owner first, preserving the
explicitly marked escaped owner. These invocation-local markers do not modify
shared backing metadata. Escaped owners last until invocation settlement or the
scalar root's aggregate cleanup boundary, where no source reference can remain.
Cleanup removes escaped owners while preserving ordinary-allocation leak checks;
existing admission charges remain conservative. This is a safe lifetime bound,
not a claim of minimal retention for every captured alias. Fault precedence,
cancellation checks and scheduler resource limits remain in force.

The private host/test adapter preserves managed input identity. When unmanaged
host storage could escape, it imports a managed snapshot; such foreign snapshots
are per argument and do not promise preservation of aliases between foreign
arguments. Read-only queries and proven fresh transforms borrow their inputs
without this copy. Argument extents are validated before any import allocation
or payload read. This adapter is not a stable public foreign-function interface.
The new private runtime helpers require matching compiler/runtime bundles.

Numeric and Bool array names now support replacement in synchronous, non-fallible CPU
functions. Their lengths may change; their element identities remain fixed.
Native branch/match results and while-loop arguments carry actual memrefs, rather
than descriptors hidden in scalar cells. This lets ownership conversion reclaim
discarded fresh buffers while keeping saved aliases alive. Every mutable-array
read receives a fresh dynamic extent identity, preventing stale shape equality
assumptions across replacements or backedges. The source access proof joins outer
array origins through branches and solves a bounded loop fixed point, including
the zero-iteration edge. A replacement can acquire fresh writable storage, but
cannot launder a readonly parameter/capture into write permission.

Proven closed function-value target sets can call their typed native entries for
numeric/Bool-array results, preserving ownership information across functional calls.
Arguments are evaluated once before dispatch; selected capture layouts are read
and validated as for the existing callable ABI. Open/large target sets retain the
existing generic callable ABI and its conservative escaped-result lifetime. This
is an implementation choice, not a source call-count or recursion limit.

Bool arrays use memref<i8> internally, with checked logical reads and normalized
0/1 byte stores. Opaque boundaries retain the canonical contiguous descriptor,
including an all-zero empty value. Typed native branch/loop and closed callable
paths expose their lifetimes to ownership conversion; pure functions require no
task runner. The checked helper ABI is marshalled from its actual LLVM signature,
so byte-backed arrays cannot be confused with scalar or aggregate arguments.
Numeric/Bool array replacement across CPU task/expected-error coordination uses
transactional frame scopes as described below. Merely copying descriptors into
reusable slots would keep distinct discarded arrays alive. Default invocation
quota reservations remain cumulative, including after explicit frees. The numerical loop qualification measures actual
live owners separately from those cumulative reservations. Mixed GPU host frames
use the same native array scopes, as described below.

Flat numeric and Bool storage has no owned child references. String, record and
function elements do: overwriting them after an alias crossed an invocation
boundary requires the backing to retain newly installed children. Context-local
dependency lists alone are insufficient. Such writes must remain rejected until
their dependency updates are implemented and qualified; immutable transforms
remain available. No nested copy-depth policy is implied by this migration.

Element assignment evaluates its destination once, evaluates and checks its index,
then evaluates the RHS and performs the write. A distinct checked-index operation
preserves that order when an await splits execution into continuation steps. The
store sink also validates its index because private continuation kernels receive
normalized scalar arguments. Failed RHS evaluation performs no store; earlier
completed writes are not rolled back on fault or cancellation. Assignments remain
statement-only, using a separate final function result as loops do.

Native CPU programs and mixed Metal/Vulkan/CUDA host continuations support
numeric/Bool arrays through the same typed CPU lowering and ownership runtime.
GPU kernel closures reject element stores. The mixed supervisor's remaining
carrier limits are listed below; they do not grant mutation inside device kernels.

## Access analysis

The spelling is `fn edit(mut values:[int])->int`, with the corresponding
function type `fn(mut [int])->int`. The marker describes access to the array's
backing, not assignment to the parameter's local name. Ordinary parameters and
all captures remain read-only access paths. A mutating contract must survive
function values, record fields, arrays of functions and indirect calls; erasing
the marker must be a type error rather than an implicit permission conversion.
Mutating contracts admit flat numeric and Bool arrays only.

Storage permission and storage origin are separate facts. A local fresh array
provides writable backing, but rebinding a read-only parameter does not make its
old backing writable. Aliases of local backing keep the same origin. Direct
function summaries describe which parameters a returned reference can alias and
which results are fresh. Capturing an array creates a read-only access path even
when its original local binding remains writable. Unknown indirect results must
not manufacture writable storage; a fresh `copy` can provide independent storage
when the compiler cannot establish a safe origin.

Reader tracking must follow statement evaluation order, not merely all tasks
observed anywhere in the function. Each unfinished reader keeps its possible
storage origins protected. A write is rejected when its possible origins overlap
a protected origin. Conditional normal exits join the outstanding-reader sets;
a zero-iteration loop preserves incoming readers. An actual await settlement can
remove that task's reader, while a cancellation request cannot. Structured group
settlement removes its own readers only. Summaries and control-flow joins must be
bounded, and uncertainty must yield a diagnostic rather than a weakened rule.

These rules are tested through ordinary aliases, captures, returned
arrays, aggregate fields, indirect calls, multiple readers, conditional awaits,
zero-iteration loops and cancellation. Positive tests must include independent
copies, private task allocations and writes after all relevant readers settle.
Direct-call summaries preserve returned parameter aliases and fresh backing.
Proven indirect targets use the same summaries. Unknown indirect returned-array
origins and aggregate field projections are conservative:
the checker may require an explicit copy when it cannot prove writable storage.
Different incoming parameters can alias; synchronous calls do not require blanket
uniqueness, but a task reading one can protect another potentially aliased parameter.
The bounded analysis reports uncertainty/exhaustion rather than granting access.
The representation and access proof are both required before an element store
can reach code generation.

## Sorting and repeated alias publication

`sort(array)` sorts flat numeric or Bool storage in place and returns the same
array reference. `sorted(array)` sorts a fresh copy, preserving its input.
Ordinary parameters/captures remain read-only, live task readers block in-place
sorting, and `spawn sort(array)` is rejected. `spawn sorted(array)` is read-only
with respect to its input and creates task-private writable result storage.

The initial implementation is an unstable heapsort, O(n log n) with constant
auxiliary storage. It uses ordinary numeric integer ordering and false-before-true
Bool ordering. Floating values use an explicit total-order bit key: complement
negative encodings, flip the sign bit of nonnegative encodings, then compare
unsigned keys. This preserves every input bit (including NaN payloads/signaling
bits), separates -0 from +0, and does no floating comparisons or arithmetic.
Negative NaNs precede negative infinity; positive NaNs follow positive infinity.
This is a language ordering policy, not an arithmetic relaxation or vendor-library
substitution. Native checked loop checkpoints remain active. Cancellation leaves
a partial ordering, while each completed swap preserves the element permutation.
A failed or canceled `sorted` never writes its source.

Publishing the same numeric backing repeatedly through opaque callable/record
boundaries reuses an existing invocation-local escape owner when the first
matching owner is already marked escaped. Ordinary transport retains retain their
separate release obligations. Reuse still checks cancellation and prior faults;
it never places mutable state on shared backing metadata. This bounds repeated
same-backing publication, including source loops, without hiding ordinary leaks.
Distinct fresh allocations in coordinating loops instead use the transactional
frame scopes described below; escape-owner reuse alone is not sufficient.

## Compatibility

Existing functional programs keep their observable results. Repeated local `=`
becomes reassignment instead of a duplicate-binding error in admitted contexts.
Array aliases are observable, and optimizations may reuse
storage only when they preserve both value results and storage identity. Record
field mutation, nested mutable containers, resizing, early loop exits, distributed
retry effects and GPU kernel memory semantics require explicit decisions; this
proposal does not silently admit them.


### Scoped ownership in CPU continuations

The private runtime can separate an invocation's owner list into compiler-owned
storage scopes. All scopes share the same Invocation pointer: cancellation,
source-fault precedence, native call control, poll observers, allocation-attempt
counting and task-budget callbacks remain unchanged. Lookups can resolve backing
across registered scopes; allocation, retention and explicit release target the
selected scope. Scope metadata has exclusive invocation access and can migrate
between workers with the execution lease.

A scope keeps cumulative reservations until that scope settles; freeing one
owner does not refund its quota. Replacing a value must first copy its complete
layout and dependency graph into a spare scope, then retire the old value and
work scopes and publish the spare. This is essential for recursive graph
children: a raw descriptor or a retained parent alone is not sufficient to
transfer the child's budget responsibility. Every live destination graph needs
its own complete ownership and reservation. The aggregate invocation counters
include all scopes. The default scope retains the previous settlement contract.

Runtime component tests exercise 10,000 replacement iterations, 5,000 recursive
graph publications, saved aliases, bounded aggregate reservations, failure
attempts across resets, cancellation/fault precedence, worker migration,
transport retention and early invocation settlement in all four build modes.
Generated CPU continuations now integrate task frame construction, instruction
commits, return publication, suspension and frame destruction for numeric/Bool array
replacement. Programs without this requirement retain their previous paths.
The private layout link guard advances to v4; development compiler and runtime
bundles must be rebuilt together.


Compiler value-scope storage now has a private runtime component: one reusable
work scope, a spare scope for transactional copies, and one scope per generated
value slot. Complete `ValueLayout` copies acquire dependencies before old roots
retire. Synchronous returns publish into the restored caller scope before callee
cleanup; task outcomes retain into their independent outcome invocation.

Control metadata is separate from source backing. It is not leaseable, is charged
to the same task budget, and registers an infallible cleanup callback with the
invocation. Cleanup runs iteratively from descendants to parents, including when
a native cancellation or source fault skips ordinary generated return blocks.
Metadata charges retire with those control objects; ordinary backing still keeps
its scope's cumulative reservations. Allocating control metadata inside a parent
work scope was rejected: it could free registered child scope nodes before their
destructors ran. The skipped-return regression tests cover both parent-frame
cleanup and final invocation settlement.

Each generated instruction selects a temporary work scope. Its owning writes
commit complete graphs; all branch/error, suspension and return edges restore
the previous selection. Frame layouts accept stopped results without reading
absent success payloads; ordinary transport validation remains strict. Numeric
parallel-map buffers retain their raw numeric backing before initialization.
Boolean map buffers first receive valid false bytes in checked 4 KiB chunks,
so complete frame-layout validation never reads uninitialized cells. Every
callback still writes its output before success can escape. Owned-element maps retain their raw buffer root without inspecting unfinished
cells. Each successful element is copied using its complete layout into that
builder scope; the generated store writes the normalized payload returned by the
copy, including cloned closure environments. Reusing an observation/element slot
therefore cannot retire references already stored in the initialized prefix.

Map preparation, batch join and final publication are distinct continuation
operations. Intermediate success descriptors are not passed to a full-array layout.
A failed join publishes its stopped/error layout before child groups are released;
only an empty, completed or stopped map reaches final full-layout publication.
Uninitialized cells are never treated as zero-valued closures or records. Builder
roots and every completed child graph count against the existing task budget and
settle with their frame. Runtime append scratch is bounded by the admitted frame
layouts, and byte copies retain 4 KiB cancellation checkpoints. These private raw
layouts cannot be used as ordinary task packet or public value layouts. No
cancellation-contract change.


Multi-output instructions must publish as a batch. Each output first copies its
complete graph into a temporary ownership scope while every old slot and work
allocation remains alive. After all copies succeed, the slot scopes swap and the
frame payloads publish; old roots retire together. Temporary metadata uses the
same checked control allocator and failure sequence. Duplicate slot writes are
rejected, and cancellation preserves its stopped status during rollback.
This handles cloned closure environments whose sibling outputs still refer to
the original environment. Empty arrays and supported positive-stride numeric views retain their
descriptors and aliases through the same private bridge. Boolean transport
continues to require its existing contiguous representation; negative-stride
imports remain rejected. The compiler emits this batch bridge for multi-output capture instructions.


The access proof distinguishes a loop's current allocation instances from its
previous instances. Before an abstract iteration, aliases and readers of storage
created by that loop age together into a conservative previous-instance root;
new allocations get the current root. This permits reading an old array in a
task while editing a fresh replacement at the same repeated source location.
Two aliases of the old array still overlap, and a reader of the new array still
blocks its mutation. Incoming parameter roots retain their existing conservative
alias and readonly rules. Function summaries recognize both fresh root classes.

Allocation membership follows the loop's structural expression graph, including
nested bodies and conditions. Unrelated roots are never aged when an inner loop
runs: a surrounding assignment may already have saved its destination before
evaluating that loop in its RHS. Zero-iteration edges remain in the fixed point.
All older instances at one site merge conservatively, so this is bounded source
analysis rather than arbitrary instance counting or a new runtime lock.

### Native arrays in mixed GPU host continuations

Mixed Metal, Vulkan and CUDA supervisors carry host numeric/Bool arrays as native
rank-one descriptors. They do not wrap mutable storage in immutable GPU buffers:
the existing tensor download bridge copies, which would otherwise hide edits
from saved aliases. Array operations are compiled by the same typed CPU backend
as ordinary programs. Array-to-tensor and tensor-to-array conversions still copy.
The GPU kernel subset does not gain array writes, shared mutation or new barriers.

Each host frame with array fields owns a native `ValueScopes` table. A generated
case selects temporary ownership before calling native helpers, retains a
successful output graph into its field's scope, then retires temporary owners.
Reusing a field retires its old reference, preserving other aliases and task
captures. Failed results commit an empty descriptor rather than inspecting an
absent success payload. Failed frame construction explicitly destroys any scopes
already initialized. TaskCore destroys completed frames before settling their
execution invocation, so those tables cannot outlive their context.

CPU worker packets and resumable helper packets retain array backing from their
source invocation. Array results use a descriptor layout with checked numeric
geometry or canonical Boolean bytes. Observing a settled result repeatedly retains
the same backing; it does not invent a new independent array. CPU task readers
still use the common source access proof. Resource limits are unchanged. Long
loops should close a nested structured group per batch to retire task records;
awaiting results alone does not close the enclosing group.

Array operations in the host continuation are emitted as a separate typed native
module and linked into `cpu.ll`. Pointer-only LLVM adapters load any aggregate
arguments using the actual generated C-interface types; generated C++ never
assumes the platform's aggregate argument ABI. Named CPU functions and synthesized
host operators retain correct export indices even when checking appends anonymous
callbacks. Programs without array carriers retain the existing scalar/tensor
call path and have no array scope state or per-case scope checks.

A host loop checkpoint now preserves the existing intended cancellation/fault
precedence: a recorded checked fault wins; an ordinary cancellation remains
cancellation rather than becoming a synthetic `fault(-1)`. This corrects the
supervisor's handling of the poll sentinel, without changing the language's
cancellation contract. Hardware kernels, arithmetic ordering and admission quotas
are unchanged. Placed first-class function values, `parallelMap` and owned
String/record/function array carriers remain explicit boundaries of the existing
mixed supervisor subset.

## Native lowering for lexical loop exits

Ordinary while and for statements without early exits use structured native
lowering. Loop expressions and lexical exits use the existing checked native CFG
planner. This generates LLVM code; it does not interpret loop bodies. A break
copies a completed value into the loop's result slot; normal completion evaluates
the else suite lazily. There is no uninitialized placeholder source result and
no eager evaluation of else.

These control-flow effects do not imply task coordination. Synchronous loop
functions and their synchronous callers use typed native entries, including
recursive calls, instead of recursively expanding source bodies. Small scalar
control frames use bounded native stack storage and are checked against the
existing conservative physical stack budget. Larger or owned frames use existing
invocation-managed storage. Non-coordinating loop entry avoids scheduler startup.
There is no new source call-depth limit or cancellation-contract opt-out.

Owned loop slots participate in invocation-local value scopes so overwriting
iteration temporaries retires old roots. The array proof carries distinct normal,
break and continue paths: unreachable awaits cannot release a reader, loop result
aliases preserve their origins, and readonly storage cannot become writable via a
break. Fault injection, running cancellation and bounded live-owner/group tests
qualify those edges in all four runtime modes.

Pure loop helpers in mixed GPU programs use private typed CPU exports, including
recursive helpers and Boolean results. These exports reuse the synchronous native
entries and the existing descriptor ABI; they do not start a task scheduler.
Ordinary native subgraphs keep their existing direct entry. Export-only modules
omit task resume entries and internalize private definitions, so linking a GPU
supervisor does not introduce unused TaskABI dependencies or duplicate symbols.
GPU kernels keep their existing numerical and placement rules.


## Eager collecting loops with yield

The array-producing form uses the existing loop expression syntax, without a
second `collect` keyword. A collecting expression appends every executed `yield`
to one eager CPU array. Nested statement loops target that expression; a nested
loop used as its own expression owns a separate collection. Anonymous functions
reset the lexical collection boundary. `yield` neither suspends nor transfers
control, and all successful loop backedges still discharge task obligations.

The semantic checker infers one completed array element type, or takes it from
an expected array type. The existing scalar numeric/Bool/String/record/function
array element model is preserved; this change does not add nested arrays or
tensor elements. Yielding a fallible value, task or device-resident value is
rejected. Zero executed yields produces a correctly typed empty array. Bare
`break` preserves the accumulated prefix and skips the rest of the loop;
`continue` keeps the prefix and advances the nearest loop. Mixing `yield` with
value-bearing breaks or an else result in the same collecting expression is
rejected. Existing single-result loops remain distinct and unchanged.

Native CFG collection instructions use invocation-local value scopes. A private
builder grows storage geometrically and copies only the initialized payload
prefix, polling between bounded chunks. It retains owned element graphs into a
separate scope; growing the payload retires the previous storage rather than
keeping all prior capacities alive. Publication retains a complete array before
retiring the builder. The builder is a descendant control allocation of its
native frame, so normal returns, expected-error propagation, checked faults,
allocation failures and cancellation all retire its scopes. There is no new
cancellation contract, public builder ABI, allocator exemption or lazy generator.
Numeric/Bool GPU-host carriers use the same implementation as CPU CFG entries.
Expected-error transfers record which lexical collections they leave, then
discard those builders from inner to outer before resuming the handler. An
outer collection survives a failure handled by an inner task group. Synchronous
callers with owned values use frame scopes too, even without a lexical loop;
function normalization includes compiler-generated map/generate references.
Read-lock analysis joins the alias roots of yielded owned elements, including
records and function captures, into the completed array's provenance.

The shared portable/cross-compilation loop corpus covers nested forwarding,
independent nested collectors, empty and filtered results, multiple yields,
nearest-loop exits, functions/captures, Strings, records, aliases, all ten
numeric types, Bool mutation, await-before-yield, and rejected read-lock and
function-boundary violations. Runtime probes inject allocation failures during
construction, cancel a running collection, test faults after growth, and check
bounded live buffers and complete budget release. GPU-host cases cover both
CPU helper collectors and direct supervisor collection across task waits.
