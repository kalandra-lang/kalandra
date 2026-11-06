# CPU to GPU closure transfer

Design and implementation plan for #282, following #279. This document is an
engineering contract; it does not advertise an implemented user capability.

## Boundary

A GPU task may receive a CPU-created function value only when the compiler can
prove its complete target set and every active capture has an admitted device
representation. The selected target executes on the selected GPU. A native
function pointer is never a wire identity, and no host execution fallback is
introduced.

The first boundary is CPU to GPU. Returning function values to the CPU, host
services, application-error handling, strings and custom workgroup facilities
remain separate work. Nested closures and function-valued aggregate fields need
the same proof as a direct function argument; they cannot bypass admission.

## Target discovery

Reuse the bounded whole-module function-value flow analysis. Transport needs
provenance at the value being passed, including values never invoked on the CPU,
not just the existing indirect-call sites. Unknown provenance must remain
explicit even when known compatible targets also exist. An empty target set or
an exhausted analysis budget is not an eligibility proof.

Target identifiers are deterministic within a compiled entry. Validate each
identifier against its exact callable signature and environment schema before
device invocation. Creating or transporting a closure does not execute its body
and must not make a factory a coordinating function.

## Captures and ownership

Translate each eligible environment into a typed graph node using existing
canonical device cells and target-native host field extraction. Transfer active
fields only; native padding, pointers and inactive enum payloads are excluded.
Preserve graph aliases across ordinary arguments and closure captures. Nested
environments participate in tracing and resource accounting.

The owning task retains captured backing and its read locks until settlement.
Host reassignment of an independent scalar binding does not mutate a captured
value. Captured shared arrays remain read-only on the device; explicit local
copies may be mutated under the existing rules. Admission must not erase mutable
parameter contracts or permit a mutating call through a read-only capture.

Resident tensor captures must retain the selected device and shape contracts.
Do not reinterpret a CPU tensor or a tensor on another device as a resident
pointer. Unsupported captures receive a diagnostic identifying the capture and
reason, before any partial result can be published.

## Runtime contract

Use bounded existing capture, graph transfer, arena allocation and checkpoint
protocols. Admission failure releases all acquired owners. Cancellation,
deadlines and faults retain their existing precedence. Device call frames and
the collector trace imported environments as they do device-created closures.
Concurrent invocations must not share mutable environment storage.

### Native capture extraction

The CPU carrier contains a native code identity and an environment address.
`ClosureNativeLayout.h` computes carrier offsets and the target-specific capture
struct using the target data layout and the same value representations as CPU
lowering. Tensor captures use descriptors; nested function captures remain
two-pointer CPU carriers until recursively translated. Empty capture sets need
no environment allocation. This metadata is not itself a device representation
or an admission proof.

Before reading capture bytes, resolve the carrier's code identity against the
compiler-generated target table, then use the invocation's checked environment
reader with the selected size, alignment and identity. The reader requires an
existing owner in that invocation and supports ordinary and recursive
environments. It borrows storage without allocating, retaining or extending its
lifetime; the enclosing task capture scope must remain alive through packing.
Cancellation and an existing fault take precedence over malformed metadata.

The graph packer accepts compiler-owned scalar projections for native target
identity. A projected tag still passes the canonical cell's range checks and
work/cancellation gates. Reference cells cannot use a scalar projection.
Environment references resolve through retained leases checked against the
selected code identity, byte size and alignment. Flat and recursive environments
use the same transport path; nominal-record keys remain distinct from closure
code keys. Existing graph interning preserves repeated backing identities.

Continuation imports expose an explicit original-to-imported function map.
Bridge emission uses that map to resolve actual callable symbols; source IDs
are not assumed to survive projection. Source-generated tests exercise both
ordinary and continuation-projected factories through canonical graph packing.

Host submission connects those layouts and bridges for read-only callable,
record and flat-array parameters with scalar companions. Scalar, flat-array
and non-callable record results use their typed return transports.
The compiler derives provenance from the original spawn arguments, emits the
selected device bodies, and retains native captures through the task packet.
All owned inputs share one graph directory, including callable-containing
records and arrays also captured by closures. CPU-resident tensor captures are
converted from their native descriptors to shape-prefixed device payloads.
Resident tensor result integration and backend/lifecycle qualification remain requirements
before this PR is ready.

### Device import representation

An imported closure uses a tagged carrier with guarded references to its
target-specific environment node. Device-created closures retain their existing
inline capture representation. Indirect dispatch validates the carrier layout,
payload length and target tag before selecting a body. Capture extraction for
an imported target additionally checks the environment layout and length.
Both forms invoke the same checked device call frames and source function body.

The collector traces the imported carrier's active environment and that
environment's owned captures. Keeping the environment as a separate graph node
allows repeated capture references to retain their original graph aliases;
import does not rebuild every record reachable from a closure. This changes an
internal device representation, not cancellation checkpoints or source semantics.

`DeviceClosureImport` carries compiler-owned layout metadata into numerical
program and shader emission. Emission tests exercise callable graph parameters,
capture dispatch and rejection of malformed metadata. The maintained
`gpu-host-closure` and `gpu-host-closure-nested` fixtures exercise real CPU-created
closures on Metal against CPU oracles. They cover array captures, nested closures,
named-versus-captured target selection and concurrent submissions. Joined-root
fixtures add callable record fields, shared arrays, and empty arrays. Runtime
wire tests assert shared node identity, and negative source tests reject mutation
or sorting of captured backing while the GPU task holds its read lock. These checks
do not replace the remaining ownership/fault matrix and backend qualification.

`ClosureGraphSchema` combines ordinary values, callable carriers and selected
environments into one directory. Callable signatures remain distinct, while
array layout keys ignore static length refinements so an array shared through a
capture and an ordinary field retains one graph identity. Callable fields are
borrowed views within their retained parent; environments use checked leases.
The generated environment offsets are exercised by source-compiled factory
tests. Host policies use these schemas directly; generated LLVM bridges compare
actual continuation callable identities and export environment identity getters.
Source IDs are canonical wire tags, not native addresses. Device entry planning
can select a pure function from a module containing unrelated host placement;
placement instructions inside the selected device graph remain unsupported.

### Returned array aliases

An array returned from a captured graph uses a tagged invocation-local origin
handle. The export bounds check verifies its arena range and matching root;
the host then resolves it exclusively against that invocation's retained graph
origins, including exact numeric layout, element count and native stride.
Unknown origins fail before publication. A captured return reuses the original
native descriptor; fresh device arrays still receive new backing. Record
results use the existing graph envelope and origin-content validation.

Source tests mutate returned backing after `await` to verify alias preservation,
and compare fresh computed returns against CPU execution. Slicing retains its
existing language semantics: `slice` creates a new array, which the closure and
its returned value can subsequently share.

### Tensor payload conversion

Captured CPU tensors use the existing local-device tensor representation: rank
extents followed by zero-extended numeric bit words. Rank zero, empty extents and
ranks through eight use the same conversion. The resolver verifies ownership,
offset, strides and allocation bounds before reading data. It copies logical
row-major elements, preserving integer and IEEE bits without arithmetic.
Repeated identical descriptors share a cached node; different views retain
separate shape/stride interpretations. No host address enters the wire.

Temporary tensor payloads share a word/work allowance with the capture resolver.
Conversion polls cancellation every 256 elements and never caches partial
results. Runtime tests cover strided views, invalid bounds/strides, budgets,
cancellation during copying, rank zero, empty tensors and repeated-view identity.
Physical Metal fixtures compare CPU-created tensor closures, including record
fields and rank-eight captures, against CPU execution with host sanitizers.

## Implementation sequence

1. Expose value-level target provenance and test unused values, factories,
   conditionals, aggregate projections, nested captures, unknown sources and
   budget exhaustion.
2. Plan eligible closure environments and target tags; integrate native host
   extraction with canonical graph schemas, without opening admission early.
3. Connect entry transport to device environment construction and indirect
   dispatch on Metal, Vulkan and CUDA.
4. Qualify ownership, read locks, mixed argument/capture aliases, cancellation,
   partial admission failure and concurrent calls.
5. Add runnable user docs and editor diagnostics. Run physical Metal, NVIDIA and
   Intel Vulkan, and CUDA correctness tests, then installed-package and hosted
   platform checks. Windows physical GPU execution remains explicitly
   unqualified until #276 is completed.

Each stage preserves rejection of unsupported boundary values until the full
transport path is implemented and tested. Compare device outputs with CPU and
independent expected results; compilation alone is not device qualification.
