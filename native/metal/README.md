# Apple device, transfer, and compute runtime

This optional native C++23/Objective-C++ library implements retained tensor storage and asynchronous CPU↔Metal transfers through the existing task scheduler. It has been exercised on an Apple M2 Max. The source compiler dispatches strict typed numerical graphs through managed tasks for all ten numeric types and tensor ranks zero through eight. See the [current typed program contract](../../docs/design/native-gpu-typed-programs.md); the sections below also retain the earlier binary32 integration history.

## Build

Use the existing pinned native Clang/LLVM toolchain and the system Foundation/Metal frameworks:

```sh
cmake --preset native -DKALANDRA_BUILD_METAL_RUNTIME=ON -DKALANDRA_BUILD_METAL_PROOF=OFF
cmake --build --preset native
ctest --preset native
```

The runtime does not require SPIRV-Cross, SPIRV-Tools, or the offline Metal shader compiler. Those are dependencies of the separate [shader toolchain proof](../metal-proof/README.md). Both options default to off in a fresh CPU-only configuration. Execution of the Metal tests requires access to an actual Metal device; missing device access is a test failure, not a skip or CPU fallback.

The CPU-only suite has 47 tests, including platform-independent external-completion and allocation-lease coverage. Enabling the transfer runtime adds four tests; enabling both Metal options also builds the optional source compiler and its source/CLI conformance suites. The transfer tests build the bridge, scheduler, and harness in debug, optimized, ASan/UBSan, and separate TSan variants. Metal API validation is enabled. Host sanitizers do not instrument GPU/driver accesses, and no shader executes in the transfer tests. The separate compute tests execute the MLIR-generated strict shader on the GPU.

## Private native interface

[Metal.h](Metal.h) exposes ordinary C++ values, not Objective-C objects or shader parameters:

- `Runtime::create` snapshots Metal devices sorted by registry ID and retains a command queue for each. The default index is zero unless configured; indexed/default selectors refer to that same snapshot. Missing devices and invalid defaults/indices return an explicit error.
- `hostTensor` checks rank (up to eight), nonnegative dimensions, byte-size arithmetic, and exact supplied byte length, then copies into an immutable host backing. A zero extent makes the payload empty even if an unused dimension is enormous. Rank zero represents one scalar element. Supported transfer representations cover all ten native numeric types; storage support does not enable source arithmetic.
- `Buffer` retains its backing through copies. Shape, format, and logical byte size are immutable. A host byte view is read-only and borrows from the buffer; the caller must retain it while using that view. A device buffer rejects host byte access. Raw Metal handles and writable backing pointers are not exposed.
- `upload` and `download` return ordinary `TaskHandle`s. Host callers submit to their root groups; native task entries can spawn transfer children in their own groups. Use `bufferLayout()` with the existing task observation/read protocol. Its copy function constructs a Buffer in uninitialized aligned output storage, as required by `ValueLayout`; callers must destroy that constructed value.
- An already-resident transfer returns a retained alias without submitting a command. Other transfers allocate fresh destination storage and use explicit private Metal buffers plus shared staging. Device buffers from another runtime registry and foreign target handles are rejected by this first bridge, even if the physical GPU is the same. General cross-registry/device transfers remain unsupported.

The first bridge is a trusted compiler/runtime C++ interface. Input spans must actually reference valid memory; checking their declared length cannot validate forged native pointers. It is not a public memory-safe FFI or a stable compiler-generated GPU ABI. Its Buffer ownership is implemented by retained opaque backings and the task value-layout destructor; it does not pretend that device buffers are allocations in the CPU `Invocation` ledger.

## Managed compute

With both Metal options enabled, the build produces `native/metal-proof/runtime.metallib` through the existing strict integer MLIR → SPIR-V → SPIRV-Cross → Metal pipeline. Its dynamic arrays replace the numerical proof fixture's fixed capacity. The runtime itself still loads precompiled libraries and does not depend on shader compiler tools at execution time.

```sh
cmake --preset native -DKALANDRA_BUILD_METAL_RUNTIME=ON -DKALANDRA_BUILD_METAL_PROOF=ON
cmake --build --preset native
ctest --preset native -R 'native-metal|metal-runtime-artifacts'
```

`loadBinary32Kernel(path, device)` loads the trusted generated artifact and checks its entry point, four buffer bindings, element layouts, and minimum workgroup capacity using Metal reflection. The artifact test separately checks the SPIR-V entry/workgroup, runtime-array layout, Int64 operations, and absence of floating arithmetic. Reflection cannot prove an arbitrary shader's numerical behavior or memory safety. This is a private compiler/runtime interface, not an untrusted shader loader or a stable extension ABI.

`compute(core, group, kernel, operation, left, right)` and the native-task overload return ordinary buffer-valued tasks. The first subset accepts equal-length rank-one float32 device buffers in the kernel's own runtime/device registry. It supports addition, multiplication, and separately rounded `(left * right) + left`; these private operation selectors are not new language syntax. Wrong ranks, formats, shapes, operation tags, empty handles, host inputs, and foreign registries fail explicitly before command submission. There is no upload or CPU fallback inside compute. Length is bounded by uint32 indexing, the byte budget, and Metal buffer limits; no 4,096-element fixture limit remains.

Every result has a fresh immutable device backing. Await/read does not download it; an explicit download is required for host access. Empty vectors produce an empty device result without dispatch. Nonempty dispatches use guarded 64-thread workgroups and an eight-byte retained parameter buffer charged to the same byte budget as tensor/staging storage. Compute and transfer tasks share command limits and the external-completion protocol below. Inputs, pipeline, parameters, and unpublished output remain alive through acknowledgement, including parent cancellation and core destruction.

The shader preserves strict float32 through integer arithmetic. This establishes correctness for the tested subset, not a GPU performance claim. A narrow source compiler now generates placed functions against a separate `kalandra_elementwise` kernel entry through `loadElementwiseKernel`/`elementwise`. It shares the checked buffer geometry and completion protocol, without the numerical proof's operation selector. Checked `fromCpu`/`toCpu` descriptor interoperation now connects this storage to CPU invocation allocations. General tensor operations, complete source error mapping, and mixed-source lowering remain subsequent work.

## Completion and lifetime

A transfer or compute task prepares all storage and encoders before reserving an external completion lease. The scheduler checks cancellation at that reservation under its mutex: a cancellation already recorded prevents submission; after reservation the backend owes an acknowledgement, even if cancellation arrives before the driver receives the command. This is the submission decision's linearization point. There is no claim that cancellation can retract a subsequent Metal commit or preempt submitted work.

The completion handler records command status and signals the one-shot capability. It does not execute Kalandra code, perform the download's host copy, or reclaim a task frame. The scheduler resumes the task only after its earlier execution lease has returned. Successful downloads materialize the host result on that resumed worker; only then can the result be published. Failure/cancellation paths discard unpublished results and staging during scheduler cleanup. The private completion-hold probe can deliberately block test callbacks; normal callbacks do not wait for GPU execution or task settlement.

Frame pinning protects inputs, pipeline/parameters, staging, and destination storage until the command has finished. Cancellation/deadlines prevent successful publication and group settlement until acknowledgement and cleanup. Buffers can outlive their producing group or the Runtime facade because they retain their registry/backing. Dropping the Runtime facade also does not invalidate an admitted transfer packet. Core destruction cancels unfinished groups and waits for outstanding external acknowledgements. A backend must always signal a reserved token; losing it would prevent settlement.

The scheduler uses one event flag/epoch per admitted record and its existing bounded ready/deferred queues. Signals neither allocate an event queue nor create a thread per transfer. Duplicate, stale-generation, previous-epoch, and post-destruction tokens are rejected. A late token holds only a weak reference to scheduler state. `step` stays nonblocking; `drain` may wait on a condition variable for external completion when it still has a step budget. CPU worker threads are free to run other tasks. External waiting has no wall-clock timeout guarantee.

## Limits and failures

Each registry separately limits tracked backing/staging payload bytes (64 MiB by default) and reserved commands (64 by default). Checked buffer allocation also respects the selected device's `maxBufferLength`. Task records, groups, and frame/outcome storage retain the core's existing limits. These are complementary budgets, not a total-process or physical-GPU-memory cap. Driver pools, Objective-C/C++ control metadata, queue storage, and allocator overhead are excluded. The command reservation count drops at callback acknowledgement; staging may remain until scheduler cleanup and remains charged against the byte budget.

Allocation/encoder failures before reservation unwind without a command or published result. Command failure is an explicit failed task, never a successful zero-filled tensor. This private bridge uses `TaskFailure::Fault` with the `metal::Error` detail codes; task-table admission continues to use the existing AdmissionRejected path. Source-level mapping to the language's placement/task errors and transport of detailed Metal NSError payloads belong to the later compiler integration.

Synchronized diagnostic counters track managed backings, Metal buffer wrappers, staging buffers, payload bytes/peak bytes, command reservations, submissions, completions, and logical allocation attempts. Counters provide quiescent cleanup checks, not driver allocation/RSS measurements. Failure injection covers explicit bridge allocation and encoder checkpoints, not every hidden driver or standard-library allocation. The completion-failure probe substitutes a failed status after a real successful command; this is not a physical device-loss test. Diagnostic controls are private and are not exposed to Kalandra programs.

## Verification

The combined native suite passes on the M2 Max. The optimized external-completion and transfer tests also pass with an explicitly optimized scheduler core.

[Transfer tests](Tests.cpp) cover exact round trips for zero through 8,193 elements with 1/2/4 CPU workers, retaining float32 bit patterns including both zeros, subnormals, infinities, quiet/signaling NaN encodings, and arbitrary bits. No floating arithmetic is performed. They also cover odd-sized uint8 copies, uint32 metadata, invalid shapes/ranks/byte lengths, immutable aliases, cached task observations, device results outliving groups, and same-residency transfers without commands.

Native supervisors spawn upload and download children, suspend through await/join, and return host tensors with a one-slot ready queue. Cancellation of a supervisor with a pending device child waits for acknowledgement. Other tests cover pre-admission cancellation, held committed transfers with cancellation and deadline requests, failed completion, each explicit host/upload/download allocation checkpoint, byte/in-flight budget rejection, 16 independent transfers on four workers, cross-registry rejection, dropped facade/input handles, and core teardown with a live command. Managed buffer/byte/command and task counters return to baseline after the relevant owners are dropped.

[External-completion tests](../tests/ExternalTaskTests.cpp) separately cover worker availability, completion arriving before resume returns, cancellation before reservation, cancellation/deadline/fault while pinned, repeated suspension epochs, duplicate/stale tokens, and destruction racing a foreign completion thread. Both suites include TSan without suppressing bridge or scheduler races.

The [Apple GPU contract](../../docs/design/apple-gpu.md) tracks the next gates. Transfer correctness does not fix the original hardware shader's strict float32 failures. The [integer arithmetic lowering](../../docs/design/strict-gpu-float32.md) is now connected to managed compute tasks through the private ABI described above. A [first optional source compiler slice](../../docs/design/apple-gpu.md#first-source-to-metal-compiler-slice) now lowers explicit source transfers and placed rank-one float32 calls through this bridge. A checked dense float32 CPU tensor bridge now passes native interoperability tests. General mixed-source lowering, GPU matmul/reductions, and NVIDIA support remain unimplemented.

[Compute conformance](ComputeTests.cpp) runs the complete 583,238-pair IEEE corpus against APFloat for all three operations through actual managed uploads, dispatches, and downloads. Prefix/tail cases span zero to 8,193 elements with 1/2/4 workers, nested CPU supervisors, and a one-slot ready queue. Repeated observations and results retained after group release bring the total to 2,121,882 APFloat comparisons per build variant, with zero mismatches. NaNs compare by classification; other outputs compare exact bits. Input buffers are downloaded and checked unchanged. The debug, optimized, ASan/UBSan, and TSan harnesses use the same generated shader; sanitizers instrument host code, not the GPU or prebuilt LLVM reference library.

Compute tests also cover incompatible fixed-capacity artifacts, invalid paths/device registries, explicit admission failures, every compute allocation/encoder checkpoint, failed completion, parameter-byte and command budgets, 16 concurrent commands, cancellation/deadline acknowledgement, nested parent cancellation, pending core destruction, dropped facade/kernel/input handles, and counter cleanup. Group deadline failures remain `DeadlineExceeded`; their cancelled children follow the existing `Cancelled` task outcome contract. These are conformance runs, not benchmarks.

The [recorded compute evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/managed-compute-2026-10-15.json) identifies the tested sources, generated artifacts, toolchain, comparison counts, and complete 68-test run.

The [common compiler and CPU tensor ABI](../../docs/design/apple-gpu.md#common-compiler-entry-and-cpu-tensor-abi) documents `kalandra --target metal`, ranked descriptors, retained CPU allocation leases, full-backing byte charges for imported views, and checked copies of downloaded results into CPU invocation storage. Nonempty imports validate actual allocation membership and bounds before payload access. A retained alias survives its source invocation; a copied CPU export survives the GPU group and buffers. These are trusted immutable compiler/runtime values, not a public mutable native-pointer API.

## Source kernel signatures

The source compiler also emits `kalandra_elementwise_v2` for dense rank-one through rank-eight float32 inputs with signature-sized scalar captures. The direct Metal layout supports up to 29 contributing input tensors; larger source calls select a Tier 2 argument buffer. The former eight-scalar array cap is removed. Use `loadElementwiseKernel(path, device, ElementwiseSignature{tensorCount, scalarCount})` and the `elementwise` overload taking tensor/scalar spans. Submission copies Buffer owners and scalar bits into bounded owned arrays; the spans may be released or changed immediately afterward. Counts must match the loaded signature. All tensors must reside on its device; contributing tensors must have equal ranks and complete shapes, not just equal byte lengths. Unused tensors may have unrelated shapes. Outputs preserve the contributing domain. The older private `compute` operation interface remains rank-one.

Bindings contain packed tensor inputs, output, then a uint32 array with element count and scalar bits. Reflection checks the declared signature. Device parameter storage is four bytes plus four per scalar, charged to the tensor/staging budget and retained until completion acknowledgement. Host capture arrays use the separate `maxArgumentBytes` budget (1 MiB by default), with `argumentBytes`, `peakArgumentBytes` and `argumentPacks` counters. Requested array payload is counted; allocator/control overhead is excluded. Empty outputs require no encoded command. The existing two-input/no-scalar source ABI and proof kernel remain supported by their original overloads. See the [source scope and conformance](../../docs/design/apple-gpu.md#gpu-entry-scalar-parameters).

See [matrix helpers and resident pipeline qualification](../../docs/design/apple-gpu.md#rank-two-gpu-elementwise-helpers) for strict scalar lifting, transitive helpers, per-axis guards and source/runtime conformance.

## Reduction tasks and scalar outcomes

`loadReductionKernel(path, device)` validates the private source-generated `kalandra_reduce_sum` entry. `reduceSum(core, group, kernel, input)` and its Task overload return float32-valued tasks observed with `Runtime::scalarLayout()`. Inputs must be dense rank-one through rank-eight float32 device buffers in the loaded kernel's registry/device. Empty input returns positive zero with no command; singleton input preserves exact bits. Unsupported kinds, formats, ranks and residency fail before submission.

Nonempty reductions encode the fixed adjacent-pair tree in flat row-major order, carrying an odd element forward, and read back only the final four-byte scalar. Each invocation reuses up to two private intermediate buffers, sized for its first two intermediate levels; later levels only shrink. One explicitly serial compute encoder preserves dispatch and resource ordering across all levels. Fallible scratch/count preparation finishes before the encoder opens. Each level has its own immutable count word. Apple-family GPUs pack these into one shared buffer with distinct four-byte-aligned offsets; other GPUs retain separate offset-zero buffers. The input, kernel, intermediate buffers and scalar staging remain retained through command acknowledgement, including cancellation. Scratch, parameter and readback bytes share the existing budget; one command admission covers the complete tree and any final scalar copy. Cancellation cannot preempt the encoded tree. See [source scope, numerical evidence and limitations](../../docs/design/apple-gpu.md#rank-one-gpu-reductions).

The [scratch reuse comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-reduction-reuse.md) records memory bounds and warm timing. Scratch is local to each invocation; cancellation of one reduction does not affect another. The [serial batching comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-16-reduction-batching.md) now records one encoder for all levels, unchanged memory bounds and roughly flat warm latency; no throughput improvement is claimed for batching alone. The [parameter packing comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-16-reduction-parameters.md) records the subsequent reduction in temporary buffer objects, with unchanged payload bounds.

The [small-workload follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/small-workloads-2026-10-26.md) writes the final reduction level directly into its retained scalar readback buffer (or the fresh resident graph result), avoiding the final four-byte blit and its encoder. It allocates private intermediate buffers only for preceding levels. Empty/singleton inputs and scalar epilogues retain their earlier copy path. The output remains unpublished until command acknowledgement; cancellation, budget accounting, input immutability and the shader's adjacent-pair tree are unchanged. Existing shader bundles use the same optimization.

Matrix results can feed a reduction on the same device without downloading their elements. Only the final scalar is copied to the host. Source parameter ranks remain checked; rank-zero and non-float32 source inputs remain gated. See [rank-two reduction scope and conformance](../../docs/design/apple-gpu.md#rank-two-gpu-reductions).

## Matrix multiplication tasks

`loadMatmulKernel(path, device)` checks the private `kalandra_matmul` entry: two runtime float32 input arrays, one output array, and uint32 `[M, K, N]` parameters. `matmul(core, group, kernel, left, right)` and the Task overload accept rank-two device tensors `[M,K]` and `[K,N]` in the kernel's registry/device and return a fresh immutable device tensor `[M,N]`. Admission checks inner dimensions, uint32 indexing bounds and existing allocation/device budgets. Awaiting the result does not download it.

The optional [one/two-row hardware profile](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/skinny-projections-2026-10-26.md) on M2 Max uses 32-column tiles with eight contraction workers per cell, retaining the exact separately rounded adjacent tree and odd-tail carries. It selects plain/fused entries only for M=1 or 2, 512≤K≤16384 divisible by 32, and 256≤N≤16384. Other shapes/devices, missing entries and unsupported pipeline resources retain their earlier kernels. Current matrices paired with older fused post-maps keep the older entry's dispatch geometry. The profile adds no per-call tensor buffers, changes no arithmetic/cancellation/budget policy, and preserves immutable inputs. Two extra pipeline objects affect cold loading; complete managed measurements and the small control losses are recorded separately from preallocated kernel gains. These shape bounds are an implementation performance profile, not language limits.

[Short graph batching](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/graph-batching-2026-10-26.md) encodes up to four consecutive pure float32 matrix/map stages in one command when the graph's matrix kernel has the hardware tiled entry. An additional allowance of 64 Mi matrix products or map output elements per batch keeps larger stages on their individual acknowledgement path; this is an estimate of work, not a wall-clock cancellation guarantee. The policy applies to rank-two product/map graphs without interleaved reductions. Typed numeric programs, host decisions, reduction feedback and software-only matrix bundles retain their existing boundaries; a final graph reduction remains a separate command. Shader arithmetic, stage order and immutable outputs are preserved.

Each batch retains earlier result buffers, parameter blocks and indirect resource buffers until actual command acknowledgement. Last-use releases are deferred rather than charging away storage still used by the GPU. Before appending a stage, the runtime checks available bytes for its result and parameters; a tight budget flushes the current command so acknowledgement can reclaim earlier values. Concurrent budget reservations still pass through the normal allocator. Batching may raise peak live storage. Cancellation is checked between encoded stages and at the external-lease submission decision; an uncommitted cancelled batch is discarded. After submission, cancellation/deadline settlement waits for the whole batch, which may contain up to four stages instead of one. No submitted Metal work is preempted. These limits govern internal batching and do not limit graph size.

The shader preserves separately rounded products and the fixed adjacent-pair tree with 32 private subtree roots per lane. Empty output has no dispatch; zero contraction length produces positive zero without reading empty inputs. One asynchronous command retains both inputs, output, pipeline and 12-byte parameters through completion acknowledgement. Cancellation cannot preempt its per-cell contraction loop. See [source scope, numerical evidence and limitations](../../docs/design/apple-gpu.md#rank-two-gpu-matrix-multiplication). The original integration made no GPU speed claim; current optional profiles are qualified separately above.

Matmul bundles now include scalar, two-row and 2×2 entries generated through the strict MLIR pipeline. On the profiled Apple M2 Max, the runtime validates all three interfaces and chooses a conservative layout from the actual M/K/N dimensions. Other GPUs and shapes outside the profile use the scalar GPU entry. See the [profile and its limits](../../docs/design/apple-gpu.md#shape-selected-register-matmul). This changes kernel selection and workgroup counts without adding tensor copies, implicit transfers or relaxed arithmetic. Managed payload budgets do not account for driver pipeline/private workspace storage.

Source-generated elementwise helpers now accept strict float32 subtraction for vectors and matrices, including scalar operands on either side. Scalar negation is supported inside reachable helpers and preserves all non-sign bits exactly. Both use the common checked arithmetic IR; the GPU path lowers to integer operations. The private `compute(BinaryOperation)` proof API keeps its existing three operations. See [source subtraction and qualification](../../docs/design/apple-gpu.md#strict-tensor-subtraction-and-scalar-negation). GPU-ABI-001 and the wider arithmetic subset remain tracked limitations.

[Signature-sized capture qualification](../../docs/design/apple-gpu.md#signature-sized-gpu-captures) covers three tensor/twelve scalar entries, the 29-input direct-buffer boundary, capture allocation failures, exact byte-budget limits and completion-safe cleanup. The [compact input projection](../../docs/design/apple-gpu.md#unused-tensor-arguments-and-shape-obligations) now copies validated binding maps, retains unused logical owners and derives result shape from contributing tensors. Source guards preserve operation-derived shape obligations independently of shader liveness. The [Tier 2 argument-buffer path](../../docs/design/apple-gpu.md#tier-2-argument-buffers-for-large-gpu-calls) now handles larger contributing-input sets through one root binding, with per-invocation encoders, byte-budgeted resource storage and explicit resource residency. GPU-ABI-001 remains open for broader operations and types.

For the private indirect entry ABI, use `ElementwiseSignature{tensorCount, scalarCount, ElementwiseBinding::ArgumentBuffer}`, optionally followed by the sorted logical input map. The loader requires Tier 2 support and matching nested reflection. Submission still uses the owned tensor/scalar-span overload. The direct two-buffer proof overload cannot submit an indirect kernel. Resource-buffer storage is charged to `maxBytes`, separately from the logical capture charge in `maxArgumentBytes`.

[Whole-tensor unary arithmetic](../../docs/design/apple-gpu.md#whole-tensor-unary-arithmetic) now supports `-values` in rank-one/rank-two float32 GPU helpers. It changes only each element's sign bit, preserving payloads and shapes; double negation and unary plus preserve exact original bits. Source shape guards, owned captures, direct/Tier 2 layout selection and acknowledgement-safe cleanup remain in place.

[Strict float32 division](../../docs/design/apple-gpu.md#strict-tensor-division) now supports tensor/tensor and either scalar/tensor order, including transitive scalar reciprocal helpers. Division retains separately rounded results rather than becoming multiplication by a reciprocal.

## Typed numerical task ABI

`loadNumericKernel` checks an exact type-qualified `kalandra_numeric_v1_TYPE`
entry and its integer-bit buffer/parameter layout. `numericElementwise` copies
exact scalar words and Buffer owners into budgeted captures. Arithmetic faults
are reported through a device-written first-fault index; a failed task never
publishes its partial output. All ten formats have private task qualification.
The source compiler now uses this interface for all-ten-type maps, sums, rank-two products and composed resident programs. See [typed programs](../../docs/design/native-gpu-typed-programs.md) for the source boundary and qualification suites.

The converter packs 8/16-bit buffer elements into 32-bit words. Device backing
allocations round those formats up to a complete word and charge the physical
size, while public shapes/byte counts remain logical. Upload padding and packed
output words are initialized before shader access. Generic typed empty maps
retain an owned dummy word and dispatch uniform validation, so checked
scalar expressions cannot disappear when the tensor domain is empty. Existing
binary32 source kernels retain their separate empty-output fast path.

`loadNumericGraphKernel` additionally validates heterogeneous source/destination
formats and resident scalar roles. `numericProgram` owns a forward instruction
program and reclaims values after executed or skipped last uses. Numerical
intermediates remain resident; selected predicates and final scalar results
use asynchronous readback. Branch-local checks execute only on the selected
path. Typed multilevel reductions preserve checked tree additions and acknowledge
all pending resources before cancellation/deadline settlement. The private
[typed runtime tests](NumericTests.cpp) cover malformed interfaces, allocation
and device failures, byte/capture budgets, cancellation, deadlines and cleanup.

`prepareNumericProgram` owns immutable validated instructions, scalar schemas and
initial use counts. `executeNumericProgram` allocates separate invocation values,
use counts and captures, so the same executable supports concurrent calls. Raw
capture words follow Map instruction order; shape-check constants stay in the
plan. An empty capture span uses the values supplied at preparation. Generated
host frames cache plans by registry/device and bind scalar values on every call.

Prepared float32 programs can batch up to four proven non-faulting stages with a
64 Mi estimated-work budget. A single larger dispatch keeps its individual
boundary. Proofs follow emitted arithmetic-fault predicates, including both
conditional branches. Host RMS epsilon also requires a valid positive finite
binding. Device epsilon, checked/unproved arithmetic, readbacks and branches
acknowledge separately. Tight workspace budgets retain individual stages.
Parameters, scratch, indirect resources and deferred last-use owners stay charged
through acknowledgement; a later host preparation/shape error waits for prior
encoded work, preserving a preceding device failure. Inputs are never donated.
See [the prepared execution design](../../docs/design/prepared-numeric-execution.md)
and [lifecycle qualification](../tests/PreparedNumericProgramTests.py).


The typed runtime can also fuse an adjacent single-consumer float32 product/map
when the compiler supplies a non-faulting canonical epilogue. Both logical stages
retain their cancellation-quantum and parameter-retirement accounting. A large
fused dispatch remains isolated. Residual maps separated by shape guards retain
separate dispatches.

Invocation-local provenance permits pointwise reuse of a private last-use
buffer. Original inputs and their aliases stay protected, live aliases block
reuse, and the final result counts as an observation. Checked maps and transforms
keep separate backing. See [fusion and private reuse](../../docs/design/gpu-projection-fusion.md)
for the safety boundary and the benchmark report, including losing cases and
additional pipeline setup cost.

## Bounded device backing reuse

Each runtime owns at most 64 idle entries within `Options::maxCachedDeviceBytes`
(default 16 MiB), all charged against `maxBytes`. Shared uploads have a separate
`maxCachedUploadBytes` subquota (default 16 MiB); zero disables that class.
Setting the combined quota to zero disables all backing reuse.
`cachePreparedOutputs` defaults to true. Only full Float32 outputs of validated
pure prepared progression are eligible for private reuse. Ordinary primitives,
transforms, packed types, empty outputs, flags, scratch, staging and parameter
arenas retain their existing paths. Entries distinguish device, exact physical
byte length and shared/private storage kind.

Shared uploads overwrite logical bytes and sub-word padding. Successful eligible
prepared maps/matrix operations overwrite every physical output byte before
publishing a new immutable value. Shape/type metadata is fresh; this is backing
reuse, not memoization or an alias to old tensor contents.

The existing `maxBytes` limit charges live payload reservations and idle backing.
A hit transfers an existing reservation from idle to live. Private admission never
displaces an existing upload working set; a private entry that cannot fit beside
cached uploads is released. Upload admission and live-budget pressure prefer
evicting private entries. Shared subquota pressure evicts shared entries.
Allocations evict idle entries when doing so can make a request fit; caching cannot turn a satisfiable
live allocation into a budget error. Logical allocation attempts and injected
failures remain the same on hits and misses. Metadata and driver-internal memory
remain outside the payload budget, as before.

Admission requires last-owner retirement and zero admitted operations in that
runtime. Admission covers copying, encoding and complete GPU progression;
frame/packet owners pin backing through acknowledgement. A new task cannot
acquire the dying last owner. Under concurrent work, retirement conservatively
releases instead of caching. There is no pending-resource queue or early GPU
reuse. Aliases, cancellation and failed completions retain their ownership and
fault-precedence rules.

`Stats::bytes`, `metalBuffers` and backing counts describe live owners.
`retainedBytes`/`peakRetainedBytes` also include idle storage. Shared-only
`cachedUploadBytes`, `cachedUploadBuffers`, `uploadCacheHits` and
`uploadCacheMisses` keep their meaning; `cachedPreparedOutputBytes`,
`cachedPreparedOutputBuffers`, `preparedOutputCacheHits` and
`preparedOutputCacheMisses` expose private retention separately. Concurrent
snapshots need not describe one atomic instant.

`trimUploadCache()` releases idle shared uploads; `trimDeviceCache()` releases
both classes. Thread-safe `setPreparedOutputCaching(false)` also evicts idle
private entries. Existing live/pending owners remain valid and cannot refill a
disabled private pool. Destroying the runtime closes and trims both classes;
escaped owners can keep state alive but cannot refill the closed cache.

Bounded idle memory retention avoids repeated buffer creation/first touch.
Arithmetic, shader selection, submission/checkpoint boundaries, cancellation
copy quanta and immutable publication are unchanged. C++ Options/Stats layouts
have grown, so native clients must rebuild; generated GPU module formats are
unchanged. Performance evidence is recorded separately from correctness.

The total reservation uses atomic compare-and-exchange. Ordinary allocations
that fit and uncached releases avoid the cache mutex. An empty private-pool snapshot can miss a concurrent optional hit and use the
ordinary atomic reservation directly. A conservative positive upload-pressure
snapshot can release private backing without locking; admission still rechecks
under the mutex. Cache mutation and pressure eviction take that mutex. Capacity is reserved before publishing live
bytes and released after dropping ownership, preventing concurrent double spend.
