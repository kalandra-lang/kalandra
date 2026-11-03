# Local mutation and read-only task sharing

Accepted direction, 2026-11-02. Implementation tracked in #266. This document
specifies the intended language; it does not claim the released compiler already
accepts these examples. It supersedes the universal-immutability requirement in
the original core proposal for the local bindings and arrays admitted below.

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
  types and indirect calls. Declaration syntax is still provisional.
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

Illustrative pseudocode, pending implementation:

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

## Implementation sequence and gates

1. Restore functional construction APIs independently of mutation. `generate` is
   implemented in this branch: runtime/constant sizes, empty arrays, ten numeric
   types, owned elements, callbacks/captures, worker import, allocation failure and
   cancellation are covered. Editor completion, hover and signature help use its
   checked types. `copy` is also implemented for numeric, Bool and String arrays;
   raw-bit tests check separate backing, strided inputs, independence after source
   changes, retained results and allocation-failure cleanup. Existing runtime
   quotas apply; no unchecked allocator is added.
2. Numeric/Bool rebinding, statement `if` and `while` are implemented in this
   branch. Scalar types remain fixed. A loop has no source value and cannot be a
   function's final result. Branch/iteration declarations stay local, while writes
   to existing scalar bindings take effect in their enclosing invocation.
   Functions and closures receive scalar values; captures are snapshots. Rebinding
   a captured scalar affects only the current call, not the shared closure
   environment. Native entry-block cells promote to registers/SSA in optimized
   builds; suspended execution uses private continuation slots and explicit read
   snapshots. No scalar cell escapes as a source reference. Every successful
   backedge settles iteration-owned task obligations; zero-iteration paths retain
   incoming obligations. Completed structured groups release scheduler records
   before another iteration. Native checked loops and continuation backedges keep
   cancellation/fault checks; ordinary CLI proofs may omit unnecessary checks as
   before. `break`, `continue`, `for` and general Unit-returning APIs remain outside
   this stage.
3. Numeric arrays now use a qualified reference representation in this branch.
   The earlier immutable MLIR tensor representation could clone
   returned arrays or reuse functional results; inserting writes into it would
   violate the intended aliases. Verify identity returns, fresh transforms,
   allocation ownership and every CPU/task ABI boundary before exposing mutation.
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
The new private runtime helpers require matching development runtime bundles;
this branch reports `0.99.3-dev`, separately from published `0.99.2`.

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
Numeric array replacement across CPU task/expected-error coordination uses
transactional frame scopes as described below. Merely copying descriptors into
reusable slots would keep distinct discarded arrays alive. Default invocation
quota reservations remain cumulative, including after explicit frees. The numerical loop qualification measures actual
live owners separately from those cumulative reservations. Mixed GPU source-array
carriers remain a separate integration boundary.

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

Native CPU programs support these arrays. The existing mixed GPU supervisor still
rejects array-valued continuation carriers, including otherwise immutable arrays;
element writes do not silently change that boundary. GPU kernel closures explicitly
reject the new store operation. Host scalar-loop support on Metal/Vulkan/CUDA remains
qualified by code-generation tests. Sharing the native CPU value/ownership lowering
with the mixed supervisor remains an architectural integration task, rather than
introducing a second array mutation implementation in its generated C++.

## Access analysis for the next stage

The spelling is `fn edit(mut values:[int])->int`, with the corresponding
function type `fn(mut [int])->int`. The marker describes access to the array's
backing, not assignment to the parameter's local name. Ordinary parameters and
all captures remain read-only access paths. A mutating contract must survive
function values, record fields, arrays of functions and indirect calls; erasing
the marker must be a type error rather than an implicit permission conversion.
The first mutating contracts will admit flat numeric and Bool arrays only.

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
The representation and access proof do not enable element writes by themselves.

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
Once arrays become mutable, aliases are observable and optimizations may reuse
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
commits, return publication, suspension and frame destruction for numeric array
replacement. Programs without this requirement retain their previous paths.
The private layout link guard advances to v4; development compiler and runtime
bundles must be rebuilt together. This remains unreleased branch work.


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
