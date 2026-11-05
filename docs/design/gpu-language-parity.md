# CPU/device language parity qualification

Issue #279 is the completion boundary. PR #281's scalar checkpoints are a
foundation, not completion. Device execution means source predicates, arithmetic,
traversal and value construction execute on the selected GPU. Host scheduling,
resource admission and transfers must not masquerade as device computation.

## Implementation sequence

1. Extend checkpoint frames to plain-data records/enums: canonical field layout,
   zeroed inactive payloads, value copies, field projection and checked matching.
   Include nested values and nonrecursive helpers. Public aggregate inputs/results
   need a typed transport ABI; local-only aggregate support is an intermediate
   milestone, not full qualification.
2. Add invocation-owned device storage with stable handles, checked lengths and
   offsets, explicit read-only captures and exclusive local writes. Dynamic growth
   must checkpoint before resource admission, then resume device work. Define
   reclamation so repeated allocation in loops cannot accumulate unreachable
   backing indefinitely. No arbitrary fixed array capacity may replace the
   language's allocation/resource contract. Bounds and allocation errors discard
   partial results and preserve existing fault/cancellation ordering.
3. Lower array traversal, indexing, local/disjoint stores and mutating helper
   contracts onto that storage. Preserve alias identity for assignment; explicit
   `copy` owns independent storage. Reuse frontend sharing/exclusive-access proofs
   and reject overlapping writes rather than silently copying or weakening them.
4. Implement ordered eager `for`/`while` collection and `yield`, `generate`, array
   `map`, `copy`, `sorted` and `sort`. Include dynamic sizes, nested/filter/empty
   results, early exits, captures and distinct invocation state. Sequential
   invocation-local append is distinct from parallel compaction. Library algorithms
   must themselves checkpoint; one long opaque sort must not bypass cancellation.
5. Complete aggregate transport and owned aggregate/tensor state; specialize
   eligible function values and captures. Specify recursion independently: a
   checked invocation-owned call stack must respect memory budgets and checkpoint
   cancellation, not introduce an arbitrary source-call depth limit. Reject host
   services explicitly. Recursive owned types require an actual storage/lifetime
   representation, not host pointers embedded in device values.
6. Publish the CPU/device capability matrix and runnable user documentation;
   verify editor diagnostics and examples. Capabilities distinguish device
   execution, host orchestration, explicit target limits and unsupported features.
7. Qualify the complete corpus on physical Metal/Vulkan/CUDA, retain unfavorable
   benchmark results including transfer/allocation costs, and audit current-head
   build/package/relocation/sanitizer/cross-platform checks. Windows GPU hardware
   remains explicitly unqualified when unavailable.

## Required evidence

Each issue checkbox needs implementation and independent CPU/device oracle cases.
Cover nested traversal; assignment versus deep copy; exclusive writes and shared
reads; numerical policies; empty/filter ordering; enum layouts and matching;
owned captures; function specialization; recursive resource exhaustion; checked
bounds/overflow/zero steps/allocation errors; concurrent invocations; cancellation
before acknowledgement versus an already recorded fault; and zero retained
resources after completion, failure and cancellation. A passing compile-only job
does not prove execution. A local macOS 27 run does not prove hosted macOS 26.

Existing scalar qualification evidence is retained under ignored
`build/qualification-evidence/`. Its four cancelled hosted macOS jobs were runner
capacity failures, not executed tests. Retry that gate when capacity is available;
it must not prevent independent progress on the remaining language requirements.

No change to shared workgroup memory, atomics, barriers or launch geometry is
implied here; those kernel-authoring APIs belong to #265. Production numerical
implementations remain independent of vendor numerical libraries. Remote testing
uses only authorized project-local dependencies and removes its copies after
evidence is saved.

## Typed array input transport

Flat numeric and Bool host-array inputs use a private versioned word protocol.
Each input contains magic/version, element layout, logical length and invocation
origin ID, followed by zero-extended raw element bits. Origin IDs identify
captured aliases; native addresses never appear in shader-visible cells. Array results use a distinct checked terminal descriptor, opaque paged payload
transport, and a typed native decoder. Captured origins retain their original
CPU backing; new device allocations receive new native owners. Native pointers
are never transmitted in either direction.

The admitted GPU child retains native backing, packs at most one 64 KiB wire
page per scheduler step, and then uploads and executes. Parent `spawn` does not
wait for that packing. The existing task read borrow protects the original and
its aliases until settlement. Native resident tensors keep their explicit
transfer requirement. Captured inputs are read-only on the GPU; `copy` creates
ordinary invocation-local writable device storage.

Input-buffer byte lengths are checked and sealed in private checkpoint metadata
before first dispatch, then rechecked after acknowledgements. Device reads
validate the wire header and bounds against those sealed lengths. Tagged input
handles resolve only to retained input buffers and are never swept as local heap
allocations. Mutating a captured input is rejected; it cannot grow its read bound
by changing a wire length or checkpoint cell.

Child uploads and computation are explicitly awaited before their groups close,
so normal task observation and fault/cancellation rules remain intact. This adds
no wall-clock preemption or cancellation-contract exception. Physical input
execution, malformed metadata, failure/acknowledgement lifetimes and installed
backend checks must all qualify before this transport is reported complete.


## Typed array result transport

Only a terminal device-control instruction can export an array descriptor. Its
reserved checkpoint cells report root, payload offset, length, layout and capture
origin. The runtime validates these against the actual retained arena and input
count after a successful final source fence. Intermediate chunks must keep the
export cells zero. A malformed descriptor is not a numerical tensor.

Fresh payloads move from the arena into independently owned private result
storage in at most 64 KiB fence-complete steps. Captured aliases have a header-only
result and reuse the matching original typed capture. The admitted child then
reads fresh payload pages, assembles a versioned CPU wire and decodes native
elements with cancellation checks. Ordinary tensor transfers reject these
private envelopes. Input wire staging is retired after upload acknowledgement;
original native capture owners remain available for alias results. This avoids
retaining both input staging and fresh result decoding unnecessarily under the
unchanged task budget.

The task result layout is the existing native numeric/Bool array layout, distinct
from a resident tensor and from the private UInt64 carrier. Bool remains distinct
from uint8. Zero-array-input generators skip the capture/upload groups. Every
child is observed before its group closes. No cancellation-contract change or
wall-clock preemption is introduced by this transport.


## Aggregate boundary representation

The compiler uses one canonical device aggregate projection for local frames and
transport planning. Each numeric or Bool leaf uses one 64-bit cell; a flat array
uses one checked handle. Nested records contribute their inline child cells.
Nominal identities, field offsets, owned leaf positions and enum payload tags
remain explicit. Recursive nominal graphs require references and tracing; the
inline projection rejects them rather than expanding indefinitely.

Native layout is a separate projection of the same checked nominal fields.
`TaskValueTypes` and LLVM's target `DataLayout` provide allocation size,
alignment and field offsets, agreeing with the synchronous CPU type emitter.
An array descriptor is native metadata, not its device representation. Padding,
native addresses and inactive enum payloads must not be serialized as cells.
Nested enums require all enclosing active-payload guards when traversed.

The private CPU pointer bridge adapts native aggregate parameters to slot
addresses and native aggregate returns to an explicit first output address.
Generated C++ therefore does not depend on platform-specific by-value struct
classification. Narrow scalar ABI extension attributes stay on their actual
scalar arguments; by-value memory/capture attributes are not copied onto the
pointer wrapper. Existing scalar and array bridges use the same adapter.

These components establish layout and native calling convention only. They do
not yet admit aggregate captures/results or provide their retained ownership,
wire graph transport or publication. Those remain required before aggregate
CPU/GPU boundary support can be reported complete. Existing cancellation,
fault precedence and checkpoint contracts are unchanged.


## Native aggregate values in mixed programs

The supervisor stores CPU records/enums in opaque slots whose size and alignment
come from the native CPU layout. Typed CPU kernel exports use the pointer bridge;
existing task payload-copy callbacks retain their active owned fields before
publication. CPU child tasks and resumable helper packets use the same native
payload layout. Ordinary synchronous CPU matching/recursive helpers remain
native calls rather than being recursively expanded into host continuation code.

Native layout is independent of device eligibility: CPU record factories can
retain Strings, record arrays and recursive links while unrelated GPU work runs.
Mixed semantic validation permits that coexistence; each GPU entry still has to
pass the device compiler's own value and operation admission. Standalone host
String/function/application-error carriers need their own typed adapters; they
must not be declared as numeric C interfaces.

The two CPU LLVM projections preserve checked source nominal identities. Their
address-significant recursive type tokens are joined before linking. Synthetic
closure-environment type tokens remain private to their projection. Native field
schemas and enum payload tables must agree before a record crosses a bridge.

A failed/cancelled aggregate result has no success payload. Its ownership slot
is discarded without traversing a zeroed enum discriminator or nonexistent
pointers. Discard releases the previous value and preserves other rooted aliases;
ordinary fault/cancellation precedence and final task settlement remain intact.

### Whole-record GPU inputs

An eligible record/enum entry is normalized to a checked typed wrapper with
numeric/Bool scalar and flat-array parameters. The wrapper reconstructs the
source record on the device and calls the original function there. An iterative,
budgeted occurrence tree keeps repeated fields distinct, including multiple
occurrences of the same nominal type. The ordinary device program and checkpoint
planner still execute the numerical body and track live owned leaves.

Host extraction uses exact target-native field offsets and carries the enclosing
activity condition down each path. It validates an active enum discriminator
before testing its payloads; inactive nested discriminators and descriptors are
never read. Inactive leaves supply neutral scalar/empty-array inputs. Selected
array descriptors retain backing through the existing native value scopes, and
the flat input job keeps its origin deduplication, readonly and acknowledgement
contracts. A returned captured array therefore retains its original CPU alias.
No native pointers or padding become device wire values.

Recursive graphs, Strings, general function values, recursive calls and tensor
state remain separate qualification work. The normalized entry accepts scalar
and flat-array peers alongside eligible aggregate inputs and results.

### Whole-record result transport

The result schema comes from the checked nominal type. Its device projection
identifies the scalar cells, enum tags and array handles. Its native projection
identifies where decoded fields belong in the result slot. The transport uses
the declared schema to validate numeric widths, Bool values and active enum
payloads. Inactive branches have no region references.

The device enumerates active array leaves before exporting the result. An
external handle names the original captured array. Each distinct fresh backing
gets one result region, so two leaves that name the same backing remain aliases
and equal contents from separate allocations remain independent. Region
references carry the checked view range. The result frame keeps the active
owners rooted while the heap services allocation or growth requests.

The terminal export reuses the flat-array transport as an opaque word
envelope. The device constructs its fixed-schema cells and copies fresh regions
in bounded checkpoint steps. The existing export service transfers that envelope
in acknowledged pages. The host validates tags, layouts, origins and ranges
before filling an owned native result slot. Fresh allocations stay private until
all active fields have decoded and the task publishes the result. Faults,
cancellation and partial decoding discard the private result using the existing
task settlement rules.

The initial acceptance case returns an enum containing one captured array, two
fields sharing a fresh copy and a separate fresh copy. After awaiting the task,
the caller changes the original array and one shared field. The native result
must observe both changes through the corresponding aliases and preserve the
independent copy. The same program exercises a second return with the inactive
enum branch and has a CPU oracle of 983 with one and four workers.

### Recursive graph transport design

The graph directory identifies allocations independently of their contents.
References use one-based indices into the directory, with zero reserved for a
nullable edge. A layout describes either a single boxed value or repeated
array elements. The directory gives the layout and element count, and the
reader derives contiguous payload offsets from the schema. Each reference
names an expected target layout. Shared references retain their identity
because every edge refers to the same directory entry.

Validation walks active fields in parent-to-child guard order. It checks
canonical scalar widths, valid enum tags, reference targets, exact payload
length and reachability. A work budget bounds the walk and cancellation can
interrupt it. The structural validator can walk cycles without revisiting an
allocation. The boundary must separately establish which cycles the source
ownership model permits before materializing a native graph.

The prototype validates self-contained graphs and packs retained native
backing. The capture resolver checks nominal box identity and descriptor ranges
against the invocation ledger before reading fields. It retains leases for
the duration of packing. A generated scalar-result task can capture one
recursive record or enum alongside scalar arguments. The child retains its
native input, packs an arena image and uploads it through the existing array
channel. Packing currently checks a one-million-word image limit and a
separate work budget before publishing a device value. Fresh recursive results use a terminal device trace. The exporter numbers
reachable allocations, writes their canonical graph into the paged result
channel, and the native decoder builds dependency-owned storage before
publishing the root. Results that refer to captured backing still need
origin metadata to recover their CPU aliases.

A validated graph can be relocated into a private arena image before upload.
This step converts directory references to device handles and writes allocator
headers. It copies canonical values without evaluating the source program.
The collector can follow relocated aliases and reclaim the graph after its
roots retire. The initialization continuation requests arena capacity, copies
one word per checkpoint, and publishes the checked root to the device frame
after staging completes. It resumes through the existing growth handshake.
