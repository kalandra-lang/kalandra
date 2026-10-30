# Apple GPU backend contract

For the current end-to-end source contract, see the
[test-backed capability matrix](native-gpu-capabilities.md). Historical numerical
proofs below remain separate from current source admission. In particular,
[GPU-HOST-001](native-gpu-capabilities.md#gpu-host-001-repaired-high-arity-source-admission)
records the repaired CPU-bridge stack-budget rejection of older high-arity source examples.

Status: executable shader toolchain evidence, managed device/transfer/compute tasks, and a first optional source-to-Metal compiler slice. Apple Silicon/Metal is the first GPU target; NVIDIA/CUDA on Windows follows under a [separate roadmap](../native-cuda-roadmap.md), excluded from Apple/main-roadmap completion. Metal source support is an optional native compiler target; the released JavaScript prototype is unchanged.

This implements the direction in [placement and transfers](placement.md), [tensor semantics](tensors.md), and [numeric semantics](numbers.md). Those language contracts remain authoritative. The [Metal toolchain proof](../../native/metal-proof/README.md) now executes generated shaders on an Apple M2 Max; its direct hardware path exposes a strict float32 compatibility gap. A separate [integer lowering](strict-gpu-float32.md) now passes scalar add/multiply conformance on that device.

## Compiler boundary

Keep the typed scalar/tensor IR and target admission shared. CPU, Metal, and future CUDA backends consume the same logical operations and source numeric policy. Device launch layout, buffer binding, and synchronization are backend details; source does not acquire Metal buffer indices or NVIDIA block syntax.

For Metal, use explicit GPU kernel IR, conversion to SPIR-V, SPIRV-Cross to Metal Shading Language, and offline compilation to a Metal library. The proof establishes this route starting at handwritten GPU IR. The optional source compiler now lowers a small rank-one elementwise subset with guarded indexing and explicit placement. General tensor-to-GPU scheduling and integration with the CPU tensor ABI remain subsequent work. Do not serialize CPU memref descriptors into shader parameters or expose arbitrary host pointers to a shader.

A compiled kernel artifact must carry a revisioned interface manifest: entry point, logical parameter order, Metal buffer slots, scalar widths, tensor rank/element format, shape/stride requirements, launch dimensions and workgroup size, required device features, and numerical policy. Validate the interface against reflection and bind against the manifest. The proof fixes one interface and checks it; it does not yet implement a general artifact format.

The first production subset should use dense float32 tensors and supported scalars, with explicit bounds checks and fresh immutable outputs. Pure ordinary helper calls follow the selected device. Transitive admission rejects host effects, nested spawning, arbitrary callbacks, unsupported formats, and unsupported numeric policies before GPU submission. Float64 must not become float32. Wider types, general control flow, reductions, matmul, and tensor acceleration need their own capability and numerical evidence.

## Device selection and admission

`devices.gpu()` selects one stable configured device, falling back to exposed index zero only when no default is configured. `devices.gpu(index)` chooses an exposed device; do not change identity between calls to balance load. The runtime retains the selected Metal device identity for the application. Missing devices, invalid indices, unsupported kernels/formats, and residency mismatches fail admission with the placement/task error contract. There is no automatic CPU fallback.

Before allocating or submitting, validate kernel/interface revision, device identity, tensor residency, rank/shape compatibility, checked dimension/byte/index arithmetic, launch limits, device buffer-size limits, and numerical capability. Reject unsupported inputs without partially publishing a result. GPU indices may use 32 bits for this first subset only if host checks prove every extent, offset, and launch index fits; CPU dimensions do not implicitly truncate. The proof's 4,096-element limit belongs only to its fixed fixture.

## Storage and transfers

Represent device tensor storage by an opaque, retained backing object associated with a specific device. Logical tensor type remains independent of residency. A descriptor records immutable shape, element format, checked capacity/offset, and residency; clients cannot forge an MTLBuffer or host pointer into it.

`transfer(tensor, target)` returns a task under the existing group/error rules. Awaiting it produces a tensor available at the destination. Awaiting a device tensor result completes computation without implicitly downloading it; explicit CPU transfer permits host indexing. Small scalar task results are copied at completion as specified by placement.

Begin with private Metal buffers and explicit staging copies. This costs copies on unified memory but gives the first implementation a clear boundary to verify. Later shared-storage and redundant-transfer optimizations must preserve logical residency, immutable aliases, synchronization, and lifetime behavior. The [native transfer runtime](../../native/metal/README.md) now implements opaque retained host/device backings, private buffers and staging, with checked shape/byte admission. This is a private C++ task bridge; source lowering and the general tensor ABI remain to implement.

Retain inputs, output, staging storage, the kernel/pipeline, and command state until GPU completion. A dropped source binding, failed parent, or cancelled task cannot release storage still referenced by submitted commands. Publish an immutable result only after successful completion and status checks. Completed tensor storage can outlive its task group through normal managed ownership. Failure before submission unwinds locally; failure after submission settles only when outstanding GPU access has ended.

## Completion, cancellation, and errors

Do not block a CPU worker awaiting a GPU command. Submission yields a pending task; a Metal completion callback queues a bounded scheduler event. The callback does not execute Kalandra handlers or resume source continuations directly. The scheduler consumes the event, checks completion/error/cancellation state, publishes at most one outcome, and settles owners under the existing task rules. The shader toolchain proof still uses a host semaphore as a harness. The separate native transfer runtime now uses scheduler completion capabilities, without a semaphore or waiting CPU worker. Download host copies and result publication run on the resumed scheduler task.

The scheduler completion reservation is the submission decision: cancellation already recorded at that point discards unsubmitted work. Once reserved, the backend must acknowledge completion even if cancellation races the subsequent Metal commit. After that decision, cancellation/deadline marks the task as unwanted and prevents successful outcome publication, but does not promise to preempt a running shader. Command references stay alive until completion; settlement and acknowledgement wait for safe cleanup. Cancellation latency can therefore include the outstanding dispatch. Kernels needing tighter bounds must be split into bounded dispatches before such a guarantee is offered.

Allocation, upload, pipeline creation, submission, and command-completion failures map to the established task/fault model while retaining backend diagnostics at the reporting boundary. Do not invent shader-side C++ exceptions or treat a failed command as a valid zero tensor. Size guards and shape errors occur before execution wherever possible. General device-reset/crash containment is not established by successful command-error reporting.

## Strict arithmetic remains an explicit capability gate

The accepted baseline preserves float32 subnormals, signed zero, infinities, NaN classification, separate product rounding, and the specified reduction tree. The M2 Max proof passes finite/separate-rounding samples but fails subnormal arithmetic. Its decorated SPIRV-Cross path also changes a signed-zero case. The compiler-controlled diagnostic fixes that case, but still fails subnormal samples even with fast math and contraction disabled.

Consequently, neither direct hardware proof variant qualifies as an implementation of general strict float32 arithmetic. The source compiler must not admit it as one. This is a backend capability failure, not a reason to change the numeric design implicitly or inspect only current runtime inputs and assume the rest are safe.

The subsequent [strict binary32 lowering](strict-gpu-float32.md) now implements scalar add/multiply through integer MLIR operations, including gradual underflow, signed zero, nonfinite cases, and nearest/ties-to-even rounding. CPU LLVM and Metal differential tests pass against LLVM APFloat and CPU hardware. It requires the strict artifact's Int64/MSL 2.2 profile. The private native bridge now uses this lowering for managed rank-one elementwise compute tasks with explicit transfers, checked bindings/residency, retained parameters, and cancellation-safe acknowledgement. Source placement and unsupported arithmetic/tensor kernels remain gated.

The integer implementation preserves the existing contract for its tested subset; no alternative numerical policy is selected. Further operations require corresponding lowering and evidence. Hardware/software correction costs still need measurement before claiming a fast strict backend. Merely offering a fused operation, accepting flush-to-zero as a default, or substituting a vendor matmul would change semantics and is not authorized by this proof.

## Implementation sequence and acceptance

1. **Toolchain proof — complete.** Actual MLIR GPU → SPIR-V → Metal compilation and execution, reflected binding checks, explicit copies, partial workgroups, unchanged inputs, host sanitizer coverage, and recorded strict-arithmetic failures. No speed claim.
2. **Device/storage runtime — implemented as a private native bridge.** Stable device selection, opaque retained buffers, checked sizes, explicit upload/download tasks, zero-sized tensors, scheduler completion capabilities, and transactional failure cleanup now have native tests, including nested groups, cancellation/deadline acknowledgement, dropped owners, budgets, and failure injection. ASan/UBSan and separate TSan cover host/bridge/scheduler code. Source-level transfers and the general tensor ABI remain a later gate.
3. **Numeric compatibility — scalar add/multiply implemented.** Integer lowering now passes IEEE edge, exponent/sign strata, cancellation, and random-value checks on CPU LLVM and Metal, including separately rounded multiplication then addition. Independent APFloat/CPU references and artifact inspection gate this subset. Other operations/types remain unsupported, and performance is unmeasured.
4. **Managed compute — implemented for the strict binary32 subset.** The generated dynamic-array shader now executes through buffer-valued native tasks. Explicit uploads/dispatch/downloads, IEEE differential checks, immutable inputs, nested groups, resource limits, transactional failures, cancellation/deadlines, and host sanitizers pass. This is a private runtime integration, not source placement.
5. **First source vertical slice — implemented for a bounded subset.** The optional source compiler now generates strict GPU MLIR from actual rank-one float32 function bodies, plus native C++ supervisor continuations. Its build driver produces a runnable executable/shader bundle. Explicit transfers, placed spawn/await, indexed/aliased targets, transitive helpers, scalar constants, repeated calls, failure injection, cancellation/deadlines, and cleanup have executable tests. The common compiler entry point and a checked CPU tensor bridge now serve this subset. General mixed-source checking/lowering remains subsequent work. See the scope below.
6. **CPU/GPU tensor bridge — implemented for dense float32 values.** The common compiler selects CPU/Metal targets, retained CPU allocation leases feed explicit GPU uploads, and checked downloads export invocation-owned descriptors consumed by actual MLIR-generated CPU code. Target-specific source lowering remains; arbitrary mixed-source programs and dynamic external-input entry points are later work.
7. **Tensor kernels and measurement — first baseline implemented.** Direct rank-one reductions and rank-two matmul preserve the accepted tree and rounding. The [initial CPU/Metal performance report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15.md) separates preallocated GPU command intervals, managed resident tasks and complete transfer paths. A [strict normalization optimization](#strict-normalization-optimization) improves matmul and reductions without changing arithmetic policy. Shape-selected register tiling now covers the measured M2 Max profile below; broader kernels and application-level measurement remain ahead.
8. **NVIDIA/Windows — separate roadmap.** CUDA/NVVM lowering, Windows runtime/toolchain integration and RTX 5090 qualification are tracked in the [NVIDIA/CUDA roadmap](../native-cuda-roadmap.md). They reuse the source placement, ownership, transfer and numeric contracts where applicable, but are not Apple backend or main-roadmap completion gates. Passing Metal tests does not establish CUDA support.

The Metal proof is optional in CMake so CPU-only development does not acquire shader tool dependencies. The shader proof contains no production code generation or source syntax changes. The separate runtime uses a generic external-completion facility for transfers and the strict elementwise compute subset. The optional source compiler adds placement parsing and target-specific source checking. The ordinary CPU compiler rejects placed calls rather than discarding the target. The released compiler and website are unchanged.

## First source-to-Metal compiler slice

The optional `kalandra-metal-source` compiler accepts the shared handwritten frontend's AST and generates a checked Metal kernel from the source function body. It checks original tensor/scalar function types once through the shared semantic stage. CPU lowering and GPU lane generation consume those same typed function bodies; GPU helper calls are inlined during target lowering. Generated MLIR is parsed and verified, then passed through the existing strict integer arithmetic lowering and the pinned SPIR-V/SPIRV-Cross/Metal toolchain. GPU constants are emitted as exact integer bits and bitcasts, preserving the float32 rounding chosen by the semantic checker.

The host supervisor currently lowers to generated C++ continuations compiled by native Clang. Transfers and placed calls become child task admissions; `try await` suspends and resumes the supervisor through the existing scheduler. It never calls a nested blocking drain on a worker. This target backend is now linked into `kalandra` and selected with `--target metal`; the smaller `kalandra-metal-source` tool remains available for IR inspection. A checked descriptor bridge connects the CPU invocation allocator and MLIR memref ABI to retained Metal Buffers. Function bodies now share the common typed IR across CPU and GPU. Supervisor placement/residency checking now uses the shared typed frontend, and its C++ continuation emitter consumes that IR. The target capability subset remains bounded; the CPU task backend and Metal supervisor now share the [continuation planner](continuations.md).

The [placed example](../../native/metal-source/placed.k) computes `(left * right) + left`:

```kalandra
fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  product = left * right
  product + left

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    left: Tensor[float32, 3] = tensor([1, 2, 3])
    right: Tensor[float32, 3] = tensor([4, 5, 6])
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    work = spawn combine(deviceLeft, deviceRight) on gpu
    result = try await work
    try await transfer(result, devices.cpu())
```

Build and run it from the repository root:

```sh
cmake --preset native -DKALANDRA_BUILD_METAL_PROOF=ON -DKALANDRA_BUILD_METAL_RUNTIME=ON
cmake --build --preset native
build/native/native/kalandra native/metal-source/placed.k \
  --target metal -o build/placed-gpu -O2
build/placed-gpu/program
```

The diagnostic entry prints exact float32 bits: `[40a00000,41400000,41a80000]`, representing `[5, 12, 21]`. This is executable conformance output, not a new language printing API. The output directory contains the executable and its offline-compiled `kernel*.metallib` files. Keep those files together when moving the bundle; the program does not require LLVM, MLIR, SPIRV-Cross, or the Metal compiler at execution time. The compiler itself uses paths from its configured native build.

`kalandra` builds the selected target; for Metal, `-o` names the output bundle directory. `--target cpu` selects the ordinary CPU path, which remains the default. A CPU-only configuration reports Metal support as unavailable rather than falling back. The inspection tool emits verified GPU MLIR and host continuation source unless passed `--build`. Both entry points support separate `--sanitize` and `--thread-sanitize` build variants; they cannot be combined. Invalid source is diagnosed before generated output is changed. The executable is replaced only after a successful link; backend build failures may leave intermediate build artifacts.

### Implemented scope

- Elementwise GPU entries have one or two dynamic rank-one or rank-two float32 tensor parameters, up to eight float32 scalars, and a result with the same shape. Direct rank-one/rank-two `reduceSum` entries return float32 scalars, and direct rank-two `matmul` entries return matrices, as described below. Transitive helpers may take scalar float32 and/or dynamic rank-one/rank-two tensor parameters and return either type, within the admitted operation subset. Every tensor input must contribute to the entry's result; scalar parameters may be unused. Operations are strict `+` and `*`, including float32 literals lifted across tensor arithmetic. No annotations or duplicate GPU implementations are needed to select placement.
- `main` has no parameters and a float32 or rank-one/rank-two float32 tensor result with `! TaskError`. Its body is one `try parallel` block. It constructs explicitly typed flat host literals, selects CPU/default/indexed GPU targets, submits transfers and placed calls, observes children with `try await`, and explicitly downloads any tensor consumed by a CPU function or returned from main. Immutable aliases and repeated calls are supported. Device indices are constant uint32 values in this slice.
- Kernel inputs/results retain dynamic array lengths in the native device ABI. The supervisor obtains its input data from literals and CPU functions. Known shapes are checked at compile time; dynamic CPU results carry checked buffer dimensions, with runtime guards on fixed-shape bindings, CPU parameters, GPU argument pairs, and main results; native kernel conformance separately exercises runtime-sized buffers. This does not claim general external tensor inputs or dynamic residency analysis in the common typed IR.
- Known shape mismatches, missing transfers/downloads, unobserved children, escaping task results, unsupported operations/types, and scalar arguments passed to tensor helper parameters are diagnosed. Scalar lifting is an arithmetic rule, not permission to erase a tensor parameter's type.
- The supervisor and its child commands use existing task/memory limits. Two uploads submitted before either is awaited need two ready admissions. Insufficient queue or device budgets fail visibly and settle their owners; no automatic admission backpressure is introduced here. A successful run requires only one CPU worker.
- `try await` propagates failures to the top-level run. Source-level recovery/matching, full placement-specific TaskError payloads, CPU functions spawning tasks, nested source parallel blocks, general tensor operations, and arbitrary mixed CPU/GPU source lowering remain unimplemented. The checked native descriptor bridge described below now interoperates with CPU invocation storage. The ordinary native compiler reports an explicit diagnostic for `on`; it never treats a placed invocation as an ordinary CPU spawn.

### Source conformance

The [second program](../../native/metal-source/composed.k) computes `left + (right * right) + 0.5` through a helper and inline `try await spawn ... on devices.gpu(0)`. It produces a different GPU artifact and results from the first program. Neither dispatches the previous proof's operation-selector shader. The source kernel loader checks the separate `kalandra_elementwise` entry, binding layouts, and kernel kind.

Both actual generated supervisors run with 1/2/4 workers and retained/cached outcomes. Tests inject every logical runtime preparation failure, hold the actual compute completion during cancellation/deadline and core teardown, and check that managed resources return to zero. The source-generated kernels also run the 583,238-pair APFloat corpus plus zero/tail/beyond-4,096 sizes through the managed buffer ABI, checking unchanged inputs and exact output bits except NaN classification. Debug, optimized, ASan/UBSan, and separate TSan variants instrument host/bridge/scheduler code; they do not instrument GPU execution or prebuilt LLVM libraries.

Compiler diagnostics run in ordinary and ASan/UBSan builds. Artifact checks verify distinct source-generated kernels, constant lowering, strict integer arithmetic, guarded workgroup geometry, and reflected buffer contracts. CLI tests build and run optimized, ASan/UBSan, and TSan programs; verify repeated placed calls and a relocated bundle; preserve an existing executable after invalid source; and require a missing GPU index to fail without CPU fallback. These are correctness checks, not performance measurements.

The [source GPU evidence](../../native/metal-proof/evidence/source-gpu-2026-10-15.json) records source/artifact hashes, both calculations, 595,730 APFloat comparisons per fixture/build variant, and the complete 80-test run.

## Common compiler entry and CPU tensor ABI

`kalandra --target metal` invokes the Metal backend as a linked compiler library. It does not launch a second source compiler process or silently infer CPU execution when Metal is unavailable. Source checking, MLIR shader lowering, and offline native linking retain the first slice's limits. This integration unifies the command entry point, not the two backends' entire typed IR or source capability set.

[Tensor.h](../../native/runtime/Tensor.h) declares ranked float32 descriptors matching the MLIR C interface: allocated/aligned pointers, element offset, sizes, and strides. Rank zero has no dimension arrays. The bridge supports ranks zero through eight with checked dense row-major layouts; noncontiguous views are rejected. GPU elementwise execution still requires rank one.

`Runtime::fromCpu(context, view)` validates shape/byte arithmetic, stride metadata, allocation membership, alignment, and the complete data range before reading payload bytes. Nonempty tensors must belong to the supplied live, successful invocation. Dense subviews may have an aligned-pointer displacement and a nonnegative element offset, provided the selected range lies within that allocation. Empty tensors do not dereference or retain data pointers. Metadata spans are a trusted native calling interface and must themselves refer to valid, stable memory during validation; this is not a public arbitrary-pointer FFI.

A successful nonempty import acquires a move-only, read-only `AllocationLease`. The runtime records each allocation's payload length and retains its backing independently of the invocation's owner list. Releasing or settling the invocation drops its owners; an imported buffer keeps the payload alive until its own last reference disappears. Copies of that Buffer share its retained storage. There is no extra host payload copy at import and no implicit GPU access to CPU pointers. Upload remains an explicit task using private device buffers and staging copies.

The native caller must preserve the language's immutable-payload contract while a lease exists. The source compiler creates the host tensor in its task invocation, imports it, and immediately releases the invocation owner; only the retained immutable backing remains. Existing mutable native pointers are not exposed to language programs. This implementation introduces no allocation-scope keyword, manual-free language feature, or user-visible lifetime parameter.

An imported view charges the complete retained CPU allocation payload to the Metal registry's byte budget, even if the view selects a small range. Separate imports can charge the same physical allocation separately; copies of one Buffer do not. This is conservative backing accounting, not physical-GPU or process-memory measurement. Allocation headers, descriptor/control metadata, and driver pools remain outside that budget.

`Runtime::toCpu<Rank>(context, hostBuffer)` checks dtype/rank and representable contiguous strides, allocates in the destination invocation, copies the completed host bytes, and returns a canonical MLIR descriptor. It rejects a device buffer before allocation: explicitly download first. Export copies so the CPU result can outlive all Metal buffers and task groups without adopting foreign allocator storage. The CPU invocation owns that result under its existing automatic cleanup rules. The bridge does not claim a copy-free round trip or reinterpret a Metal object as a CPU memref.

[Interop conformance](../../native/metal-source/InteropTests.cpp) runs CPU-owned tensors through the source-generated GPU kernel and explicit download, retires the original invocation and GPU group/buffers, then calls the [MLIR-generated CPU reduction](../../native/metal-source/cpu-consumer.k) on the exported descriptor. There are 24 shape/worker cases per debug, optimized, ASan/UBSan, and TSan variant, including zero and partial workgroups. Other probes cover dense offsets, ranks zero/two/eight, signed-zero/subnormal/NaN bit preservation, forged/foreign/out-of-range descriptors, settled/faulted contexts, byte budgets, and export failure cleanup. Platform-independent allocation-lease tests separately exercise moves, retained contexts, and concurrent final owner release.

CPU-only configuration/build and compiler smoke checks still pass without Metal/shader dependencies. Full mixed-source type/effect checking, device residency in the shared typed IR, richer CPU/GPU entry parameters, and general tensor kernels remain subsequent implementation work. This bridge establishes safe interoperability; it does not establish GPU performance.

The [tensor interoperability evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/tensor-interop-2026-10-15.json) records the compiler/ABI sources and executable hashes, 24 CPU→GPU→compiled-CPU cases per build variant, CPU-only build checks, and the complete 89-test run.

### Mixed CPU/GPU source programs

The [mixed source fixture](../../native/metal-source/mixed.k) defines CPU and GPU functions in one file. Ordinary calls run on the CPU; `spawn ... on devices.gpu()` selects the GPU. No function annotation is needed. All ordinary function bodies use the common tensor/scalar type checker and typed IR. A function may be called on CPU and placed on GPU without a second definition. GPU capability admission still rejects unsupported operations in the reachable helper closure. The supervisor does not yet use a shared placement-aware typed IR.

```kalandra
fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  product = left * right
  product + left

fn main() -> float32 ! TaskError:
  try parallel:
    left: Tensor[float32, 3] = tensor([1, 2, 3])
    right: Tensor[float32, 3] = tensor([4, 5, 6])
    gpu = devices.gpu()
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    result = try await spawn combine(a, b) on gpu
    host = try await transfer(result, devices.cpu())
    total(host)
```

This returns 38. `total(result)` is rejected: awaiting GPU work leaves its result on the device. The download stays explicit. The executable currently prints scalar float32 bits in hexadecimal, as it does for tensor elements.

CPU bridge functions accept and return float32 scalars or rank-one/rank-two float32 tensors. They can call other CPU functions, including checked shape-constrained helpers. Calls run synchronously on the current worker; compiled loops poll cancellation. Each tensor argument is copied from a completed host Buffer into an invocation-owned CPU descriptor, then released after the call. This keeps ownership explicit internally; it does not claim a zero-copy CPU call path. CPU allocation/shape faults propagate through the owning task and settle the invocation. Scalar results use a separate immutable task result layout; main can still return a host tensor.

The bundle includes `cpu.mlir` and `cpu.ll`, and links the CPU object code into the same executable as the supervisor. CPU exports use checked invocation interfaces and do not synthesize a second main. Sanitizer variants instrument the generated CPU LLVM for ASan or TSan, alongside the existing host/runtime sanitizer builds; UBSan covers C++ host/runtime code, not arithmetic already lowered to LLVM. There is no compiler-library dependency in the executable.

Current limits: CPU bridge functions cannot return tasks or tensors beyond ranks one and two; GPU entry signatures and operation admission remain restricted; CPU spawn and explicit CPU placement are now implemented in this supervisor, as described below. General tensor operations and a common placement-aware typed IR are still subsequent work. Existing strict GPU arithmetic qualification is unchanged, and performance is unmeasured.

[Mixed-source conformance](../../native/metal-source/MixedTests.cpp) checks results and repeated observation with one, two, and four workers, every supervisor invocation allocation failure up to the successful run, cancellation while GPU completion retains the frame, and resource retirement. It runs in debug, optimized, ASan/UBSan, and TSan variants. CLI tests cover empty tensors, CPU shape faults after GPU download, tensor-valued main, and executable/shader relocation. Source diagnostics reject CPU access to device tensors, bad CPU arguments, shapes, duplicate names, and shadowed function calls before writing output.

The [mixed-source evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/mixed-source-2026-10-15.json) records source and artifact hashes, CPU-only smoke checks, and the complete 94-test passing run.

### CPU tensor results

Ordinary CPU functions can now create, compute, forward, and return rank-one float32 tensors in mixed source programs. The [tensor-result fixture](../../native/metal-source/tensor-results.k) constructs GPU inputs with a CPU function, forwards them through nested CPU helpers, uploads them explicitly, runs the GPU kernel, downloads its result, and returns a CPU helper's tensor result. A CPU function can also build a tensor from a scalar computation, such as `tensor([reduceSum(values)])`.

```kalandra
fn seed(first: float32, second: float32, third: float32) -> Tensor[float32, 3]:
  tensor([first, second, third])

fn forward(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values
```

CPU tensor results use the existing MLIR ranked-memref C interface: the caller supplies a result descriptor, and the checked invocation owns its returned storage. The buffer-deallocation pipeline produces owned results, including a clone where forwarding an argument would otherwise borrow its backing. The supervisor acquires an immutable Buffer lease before releasing the result descriptor's invocation owner. This adds no result payload copy at import, but does not remove copies introduced by MLIR to establish ownership. CPU tensor arguments still use the copying descriptor export described above. Aliases and cached results retain the backing after the producing invocation and scheduler have ended.

A dynamic result's length is unknown to the supervisor checker unless the CPU type checker proves a constant. Transfers retain that uncertainty. Fixed-shape binding and main-result annotations add runtime guards; GPU calls check dynamic argument lengths before kernel admission. CPU interfaces check their declared parameter shapes. A mismatch raises ShapeFault, with no GPU fallback or automatic transfer. This extends shape checking in the target-specific supervisor; it does not yet add placement/residency to the common typed IR.

The [tensor-result conformance tests](../../native/metal-source/TensorResultTests.cpp) run the complete source program with one, two, and four workers in debug, optimized, ASan/UBSan, and TSan builds. They retain results past group/core retirement, compare repeated observations, sweep all eight invocation allocation failures and Metal preparation allocations, and check cancellation, deadlines, completion failure, byte budgets, and cleanup. [CLI cases](../../native/metal-source/VerifyTensorResults.cmake) cover empty tensors under ASan/UBSan, 65-element CPU factories feeding partial GPU workgroups, negative-zero/subnormal bit preservation, CPU-computed tensor results, and runtime shape faults at bindings, CPU calls, GPU calls, and main results. Diagnostics reject unsupported result ranks/types, fallible CPU tensor functions, oversized extents, and missing transfers before writing output.

Rank-two CPU tensor interfaces and CPU tasks in mixed programs are now implemented by subsequent increments. Broader GPU kernels and a shared placement-aware typed IR remain subsequent work. Performance is unmeasured.

The [CPU tensor-result evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-results-2026-10-15.json) records source/artifact hashes, the allocation and lifetime checks, and the complete 99-test passing run.

### Shared CPU/GPU helpers

The [shared-helper program](../../native/metal-source/shared-helpers.k) calls one `combine` definition on CPU and GPU, then passes their results to a shared `add` helper. `combine` calls both a tensor helper and a scalar helper. The compiler checks the original definitions once, preserving tensor parameter types, scalar parameter types, return types, and shape requirements. It no longer rewrites GPU function signatures into scalar functions or uses a separate AST tensor checker.

```kalandra
fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left * right + left
```

Within a mixed program's parallel block, `local = combine(left, right)` executes CPU code. After explicit uploads, `work = spawn combine(deviceLeft, deviceRight) on devices.gpu()` selects GPU execution. The ordinary CPU-only target can compile the same definition, including calls from CPU tasks. No annotation or alternate function implementation selects the target. Device tensors still require explicit download before CPU calls, and host tensors require explicit upload before GPU placement.

The shared typed IR now has `TensorBinary` for strict float32 elementwise `+` and `*`. A scalar operand lifts across every tensor element, on either side of the operator. Tensor operands must have identical rank and extents; there is no tensor-axis broadcasting. Known mismatches are compilation errors. Unresolved equality requirements propagate through helper calls and become runtime guards. The mixed supervisor checks CPU helper preconditions before exporting descriptors or executing the call.

CPU lowering allocates a fresh immutable result and emits ranked serial MLIR loops with separately rounded arithmetic. Allocation bounds, ownership, and loop cancellation use the existing checked invocation machinery. The task backend extracts these same operations as CPU kernels. GPU lowering emits one scalar lane from the typed tensor operation, then uses the existing integer implementation of strict binary32 arithmetic. Scalar helpers are permitted within the GPU closure when their operations are supported; scalar division is now supported inside these closures through the strict integer lowering. There is no CPU fallback.

Elementwise and reduction GPU entries support rank-one/rank-two float32. The mixed bridge also supports rank-two float32 tensors for direct GPU matmul and ordinary CPU calls. Ordinary CPU elementwise lowering supports ranked tensors; tests include rank-two and empty dimensions. General elementwise entries use signature-sized captures and compact bindings for tensors that contribute to the result, described below. Elementwise GPU helpers cannot introduce tensor constructors, reductions or other unsupported operations; fixed-shape refinements are checked by the host preflight; direct scalar reduction entries are supported separately below. This milestone shares function typing and arithmetic IR, not placement/task/residency IR for the whole program. Performance is unmeasured.

[Shared-helper conformance](../../native/metal-source/TensorResultTests.cpp) compares the actual compiled CPU helper and source-generated GPU kernel against APFloat for **595,723 input pairs through each of three helpers** (addition, multiplication, and their composition with a scalar helper): **1,787,169 pair/operator cases per build variant**, including zero/tail lengths, NaNs, infinities, signed zeros, and subnormals. Each CPU and GPU result must match the reference, and the two results must match each other; NaNs are compared by classification, other values by bits. The same suite checks input immutability, runtime CPU shape faults, cached results after invocation/core retirement, all 15 supervisor invocation allocation failures, Metal preparation failures, cancellation/deadline settlement, and budgets. Debug, optimized, ASan/UBSan, and TSan variants run on the Apple GPU.

[CLI checks](../../native/metal-source/VerifyShared.cmake) compile one source helper for both targets, verify tensor types remain in CPU MLIR, execute it from a CPU task, and check ranked scalar lifting, empty tensors, CPU-only capabilities, and dynamic supervisor shape guards. Diagnostics reject unsupported GPU scalar operations, scalar-to-tensor helper arguments, scalar GPU entries, fixed-shape GPU helpers, and known CPU shape mismatches before changing output.

The [shared-helper evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/shared-helpers-2026-10-15.json) records source/artifact hashes, independent addition/multiplication/composition checks, CPU-only smoke checks, and the complete 104-test passing run.

## GPU entry scalar parameters

This section records the initial entry milestone. Its fixed counts have since been removed by [signature-sized captures](#signature-sized-gpu-captures); the supported operation/type subset remains bounded.

A GPU entry can accept **one or two rank-one dynamic float32 tensors and zero to eight float32 scalars**, in any parameter order. Its result remains a rank-one dynamic float32 tensor. The same function can run on CPU:

```kalandra
fn scale(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  values * factor
```

Use `local = scale(values, factor)` for an ordinary CPU call. After uploading `values`, use `work = spawn scale(uploaded, factor) on devices.gpu()` and `result = try await work` for GPU execution. The result stays on the device until explicitly downloaded. Scalars require no transfer: submission copies their exact float32 bits into the owned task packet. Caller storage does not remain borrowed while a command is pending.

The [source fixture](../../native/metal-source/scalar-entry.k) exercises scaling, scalar-first affine transformation, interleaved tensor/scalar arguments, identity, and eight scalar parameters. It calls the affine helper on CPU as well. All tensor arguments must reside on the selected GPU. Tensor operands of the same elementwise operation must have equal extents; unused parameters need not share the result domain. Scalar-only entries, scalar GPU results other than the direct reduction subset below, and unsupported operations remain diagnostic. Both scalar and tensor parameters may be unused. Signature-sized captures below replace the initial two-tensor/eight-scalar storage limits. This is elementwise execution, without axis broadcasting.

The private `kalandra_elementwise_v2` ABI packs tensor bindings first, followed by output and a uint32 parameter array. That array holds the element count followed by scalar bit patterns, in scalar declaration order. Runtime reflection checks binding count, element types, offsets, lengths, sizes and alignment against the compiler-supplied signature. Parameter storage costs `4 * (1 + scalarCount)` bytes against the existing byte budget and remains retained through command acknowledgement. Two-tensor entries without scalars keep their existing ABI. Neither loader is a numerical validator for untrusted shaders.

[Conformance checks](../../native/metal-source/ScalarEntryChecks.h) check five CPU/GPU helpers in **676,900 cases per build variant**, using APFloat for arithmetic and exact bits for identity, including empty/tail workgroups, subnormals, signed zeros, infinities and NaNs. Identity additionally preserves exact input bits, including NaN payloads. Tests change caller capture arrays immediately after submission, reject incompatible signatures/counts/residency before dispatch, and run supervisor allocation failures, cancellation, deadlines, completion failure and cleanup in debug, optimized, ASan/UBSan and TSan builds. [CLI checks](../../native/metal-source/VerifyScalarEntry.cmake) cover CPU execution, unused scalars, placement and diagnostics without modifying existing output on rejection. [Evidence](../../native/metal-proof/evidence/gpu-entry-signatures-2026-10-15.json) records the run and artifact hashes.

Strict arithmetic implementation and its qualification are unchanged. GPU performance is unmeasured. Placement/task/residency lowering still uses the target-specific supervisor.

## Tracked follow-up: general GPU parameter layouts (GPU-ABI-001)

Status: open, with signature-sized capture storage, unused tensor parameters, operation-derived shape obligations and Tier 2 argument buffers implemented for the current elementwise subset. The former two-tensor/eight-scalar arrays have been removed from the general elementwise submission path. Layouts now derive tensor/scalar counts and source parameter order from checked signatures, copy scalar bits and retain every tensor owner in bounded arrays, and validate matching shader reflection. The Metal direct-binding layout handles up to 29 contributing tensor inputs (31 buffer entries minus output and parameters); larger calls now select one Tier 2 argument buffer. Unused logical parameters consume capture storage but no shader input pointer. Scalar capture size is governed by checked representation, device buffer size and memory budgets rather than an eight-value array.

Remaining work includes additional supported types and richer tensor operations beyond the current single result lane domain. The current float32 subset no longer has a fixed 29-contributing-input ceiling on Tier 2 devices. These are implementation stages, not universal language parameter counts. Ordinary CPU functions do not inherit the Metal binding-table limit.

Completion requires:

- Implemented for the current float32 subset: derive tensor/scalar layout from the checked function signature, preserving source evaluation order and mixed parameter ordering.
- Implemented for general elementwise submission: bounded owned arrays retain tensor owners and copy scalar bits at submission, with separate capture-byte accounting and tested cancellation/allocation-failure cleanup.
- Signature counts/source order, logical input maps and binding layouts are emitted in artifact metadata, with reflected binding/scalar layouts checked at load. Direct and Tier 2 argument-buffer layouts are implemented; broader types and operations remain future work.
- Implemented for the current subset: shape requirements come from checked operations and propagate through helper calls, including discarded expressions. Only tensors that contribute to the returned value are read by the shader. Used lane operands still require a common complete shape; general multidomain tensor operations require further lowering.
- More than two tensors/eight scalars, mixed ordering, unused scalar/tensor arguments, invalid layouts, failure sweeps and cancellation now have numerical/sanitizer evidence. A 40-tensor logical signature with one physical input and entries with 40 contributing inputs are qualified below. Broader operation-derived tensor layouts remain future work.

This does not imply that every language type can immediately execute on every GPU. Type and operation support must expand explicitly, with capability checks and numerical evidence.

Rank-one GPU `reduceSum` now has a first implementation, described below. GPU-ABI-001 remains a separate open requirement; reduction support does not turn the current argument counts into language semantics.

## Rank-one GPU reductions

A user-defined function can now run `reduceSum` on a rank-one float32 device tensor and return a scalar:

```kalandra
fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)
```

After explicit upload or another GPU computation, `result = try await spawn total(uploaded) on devices.gpu()` returns an ordinary float32 value. Small scalar results are copied at completion, so `result` can go directly to a CPU function or become a scalar parameter in another GPU call. This does not download the tensor. Tensor-valued GPU results still require explicit transfer for CPU use.

The [source fixture](../../native/metal-source/reduction.k) calls `total` on CPU, scales a tensor on GPU, reduces the device result, and passes the scalar to a CPU function. It prints `41f80000` (31) through the diagnostic executable's existing bit-output interface.

A reduction entry resolves to one checked `ReduceSum` over rank-one or rank-two float32 tensors with the balanced-adjacent, row-major policy. [Forwarding helpers](native-gpu-forwarding.md) support aliases, local bindings and imported helper calls; [fixed and dynamic extents](native-gpu-fixed-shapes.md) retain their shape contracts. [Elementwise input expressions](native-gpu-pre-reduction.md) and [scalar result arithmetic](native-gpu-post-reduction.md) now compose around that reduction, including mixed tensor/scalar parameters. Multiple independent primitives, ranks above two, other formats and general scalar-only GPU functions remain unsupported. Use separate placed calls for broader composition; their tensor intermediates stay on GPU. CPU execution retains its broader operation support. There is no CPU fallback for unsupported GPU bodies.

The GPU shader performs one tree level: adjacent pairs add with strict binary32 rounding, and an odd final element is copied unchanged. The runtime encodes successive levels in order, using one serial compute encoder and two alternating private output buffers, until one value remains. Each buffer is sized for its first use; subsequent levels only shrink. Serial dispatch ordering and tracked buffer dependencies preserve accesses when storage is reused. This matches the CPU/reference tree independently of thread scheduling. Empty input returns positive zero without a command; singleton input copies its exact bits without adding zero. Every nonempty reduction uses one command buffer and finishes with a four-byte staging copy. It neither uses a host arithmetic reduction nor reassociates the tree into scheduling-dependent partial sums.

The private `kalandra_reduce_sum` entry has input/output runtime float32 array bindings and one uint32 element-count word. Reflection validates its layout, and kernel-kind checks prevent elementwise/reduction entry substitution. The existing uint32 indexing bound allows at most 32 levels. Scratch buffers, count words and scalar readback are charged to the shared byte budget and retained through completion acknowledgement, along with the input and pipeline. For at least three inputs, private outputs hold `ceil(n / 2) + ceil(n / 4)` float32 elements, instead of retaining every tree level; a two-element input needs only one output word. Count words remain separate and immutable for each encoded level. Storage is still linear in input length, with about 25% less output scratch for large inputs. The pair belongs to the invocation, so concurrent reductions never share writable scratch. Allocation/encoder failures unwind before commit. Once committed, cancellation/deadlines prevent publication and wait for acknowledgement; the runtime does not promise interruption between encoded levels or hardware preemption.

[Original rank-one conformance](../../native/metal-source/ReductionTests.cpp) compares compiled CPU results, actual GPU results and an APFloat implementation of the accepted tree in **570 cases containing 3,264,973 input elements per build variant**, with zero mismatches. Coverage includes empty and singleton values, odd levels, workgroup/power-of-two tails through 131,073 elements, signed zeros, subnormals, cancellation-sensitive finite sums, infinities, NaNs and deterministic random inputs. Arithmetic NaNs compare by classification; singleton copies preserve payload bits. Tests also check unchanged inputs, repeated observation, invocation and Metal allocation failures, command/byte budgets, missing or incompatible kernels, invalid ranks/formats/residency, dropped owners/facades, pending core destruction, cancellation/deadlines and simulated failed completion. Debug, optimized, ASan/UBSan and TSan variants run on the Apple GPU.

[CLI checks](../../native/metal-source/VerifyReduction.cmake) cover scalar CPU/GPU composition, repeated awaits, empty input, negative zero, bundle relocation, strict artifact inspection and rejection before changing existing output. The [reduction evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-reductions-2026-10-15.json) records the numerical and lifecycle run and artifact hashes. The [initial performance baseline](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15.md) now measures reductions with and without resident inputs. Direct GPU matmul is implemented below; tensor-kernel optimization and general placement/task/residency IR remain subsequent work.

The [scratch reuse comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-reduction-reuse.md) records the runtime-only optimization, exact budget checks, independent concurrent scratch, and warm timing against the previous implementation. Shader arithmetic and the tree are unchanged. Serial encoder batching is now qualified below; broader source composition remains future work.

## Rank-two GPU reductions

`reduceSum` also runs directly on dense rank-two float32 device tensors. A matrix reduction uses the same strict shader and scalar task interface as a vector reduction:

```kalandra
fn total(values: Tensor[float32, ?, ?]) -> float32:
  reduceSum(values)
```

Reduction visits elements in flat row-major order using the accepted adjacent-pair tree. It does not first reduce each row. For example, `[[1e20, 1, -1e20], [1, 1, 1]]` reduces to `2`; reducing rows separately would incorrectly change the result to `3`. No shape conversion, implicit tensor download, arithmetic reassociation, or host reduction is involved. Either zero dimension returns positive zero without a command; a one-element matrix preserves its exact bits, including negative zero and NaN payloads.

A matrix result from one GPU task can feed another task while staying on device. In the [resident matrix example](../../native/metal-source/matrix-reduction.k), `a` and `b` are explicitly uploaded inputs:

```kalandra
work = spawn project(a, b) on gpu
result = try await work
summary = spawn total(result) on gpu
try await summary
```

The final await returns an ordinary float32 scalar through a four-byte readback. Downloading the full matrix is only necessary if CPU code needs its elements. The [CLI checks](../../native/metal-source/VerifyMatmul.cmake) compile and run this example with optimized, ASan/UBSan and TSan host configurations and 1/2/4 scheduler workers. Inspection confirms the generated supervisor has no matrix download. A relocated executable plus its two root Metal libraries runs without compiler intermediates.

Admission requires the argument rank to match its source parameter, explicit device residency, float32 storage, and a flat element count within uint32 indexing and the managed byte budget. The runtime rejects rank-zero/rank-three inputs and incompatible kernels, formats, registries or residency before submission. GPU source parameters now accept [fixed, dynamic and mixed extents](native-gpu-fixed-shapes.md); dispatch validates fixed extents before submission. Aliases and forwarding helper calls inside reduction entries are now supported by [numerical forwarding analysis](native-gpu-forwarding.md). Extra arithmetic, additional entry parameters and higher-rank GPU arithmetic remain unsupported; separate placed stages are supported. These are implementation limits.

The expanded reduction corpus compares CPU and GPU against APFloat in **653 cases containing 4,070,261 elements per build variant**, including the original vector corpus, odd matrix axes, row-boundary pairing, empty/singleton matrices, arbitrary bit patterns, signed zeros, subnormals, infinities and NaNs. Both ranks sweep every preparation allocation/encoder checkpoint and check cancellation, deadlines, failed completion, pending core destruction and retained scratch. A concurrent vector and matrix reduction verifies independent scratch while one is cancelled. Source checks reject host inputs, wrong parameter ranks, fixed GPU extents and composed reduction bodies before changing existing output. See [rank-two reduction evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-matrix-reductions-2026-10-15.json). No performance claim is made for this feature; the existing shader and scratch layout are unchanged.

## Rank-two GPU matrix multiplication

A direct matrix helper can now run on CPU or GPU from the same checked function definition:

```kalandra
fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)
```

With `input` shaped `[M, K]` and `weights` shaped `[K, N]`, ordinary `project(input, weights)` executes on CPU. Upload the two inputs explicitly, then use `work = spawn project(a, b) on devices.gpu()` and `result = try await work` to get a device tensor of shape `[M, N]`. Await does not download it. The [source fixture](../../native/metal-source/matmul.k) downloads the result before passing it to a CPU reduction; it produces `43230000` (163). A downloaded matrix can also be returned from main, whose diagnostic output prints its elements in flat row-major order.

The mixed-source supervisor now tracks both matrix axes through literals, CPU calls/results, transfers, placed calls, awaits, annotations and main results. Nested rectangular literals construct matrices; an empty outer literal uses its annotated column extent. The CPU bridge exports/imports rank-two descriptors and propagates axis-specific shape requirements. Known mismatches are compilation errors; unknown inner dimensions and fixed-width annotations have runtime guards before calls or host descriptor access. GPU runtime admission independently checks format, rank, inner dimensions, selected-device residency, byte bounds, and uint32 dimension/input/output index capacity. Inputs may alias; each result has fresh immutable storage.

A matrix GPU entry resolves to one checked `Matmul` with separately rounded products and balanced-adjacent reduction. [Forwarding helpers](native-gpu-forwarding.md) preserve local bindings, aliases, imported calls and reordered or repeated operands. Signatures can mix tensor/scalar parameters, including one tensor used as both operands. [Elementwise matrix results](native-gpu-post-matmul.md) now apply strict arithmetic to the product with scalar or matching full-shape tensor operands, using resident stages and projected direct/Tier 2 bindings. Fixed, dynamic and mixed extents preserve host-side guards. [Matrix operand arithmetic](native-gpu-pre-matmul.md) now adds resident elementwise input stages. Additional independent primitives still need separate placed tasks, with intermediates remaining on device. GPU-ABI-001 remains a separate interface scope. Elementwise GPU calls and GPU `reduceSum` retain their existing support.

The [generated kernel](../../native/metal-source/MatmulShader.h) assigns one output cell to a lane in a guarded 64-thread workgroup. It rounds every product to float32 before addition. A private stack stores aligned power-of-two subtree roots as products arrive; occupied roots combine from low to high level, placing each larger subtree on the left. This reproduces the accepted adjacent-pair tree even for odd contraction lengths, without adding an initial zero to a nonempty sum. The stack holds 32 float32 roots per lane under the current uint32 indexing bound, rather than allocating `M*N*K` global products. Strict scalar multiplication/addition use the existing integer GPU lowering; vendor matmul, hardware float32 reassociation and fusion are not substituted.

Zero rows or columns yield an empty device tensor without dispatch. Zero contraction length with nonempty output dispatches positive zeros without reading the empty input buffers. The private `kalandra_matmul` entry has two flat input buffers, output, and a reflected uint32 parameter array `[M, K, N]`. The runtime retains the input owners, output, pipeline and 12-byte parameter buffer through asynchronous command acknowledgement. Parameter/output storage shares the existing byte budget. Cancellation and deadlines prevent publication and wait for safe cleanup; a running shader is not preempted, and the per-cell contraction loop has no mid-kernel cancellation check.

[Matrix conformance](../../native/metal-source/MatmulTests.cpp) checks **676 shapes/pattern cases, 19,166 output cells and 3,621,350 separately rounded products per build variant** against both the compiled CPU helper and an APFloat product/tree reference. It covers contraction lengths through 1,025, odd/power-of-two and workgroup tails, empty dimensions, negative zero, subnormals, finite cancellation, overflow, infinities, NaNs and a fusion-sensitive case. Other checks cover immutable/aliased inputs, retained/cached results, allocation and encoder failures, command/byte budgets, invalid ranks/formats/residency, uint32 overflow guards using empty buffers, cancellation/deadlines, simulated completion failure, dropped owners and pending core destruction. Host/runtime debug, optimized, ASan/UBSan and TSan variants execute on the Apple GPU; these sanitizers do not instrument device instructions.

[CLI checks](../../native/metal-source/VerifyMatmul.cmake) run rank-two CPU factories and consumers, matrix main results, chained GPU calls, zero axes, source diagnostics and dynamic inner/width guards. [Evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-matmul-2026-10-15.json) records the run and hashes. This establishes a strict baseline. The [initial CPU/Metal measurements](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15.md) now show higher GPU matmul latency than the optimized CPU call on the tested shapes, including the preallocated device-command control. Tiling, tensor acceleration, numerical-policy alternatives and broader source/parameter layouts require separate implementation and evidence.

## Initial CPU / Metal performance baseline

The [benchmark report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15.md) records three processes with excluded warmups, seven samples per cell and independent validation of every result. It distinguishes checked CPU calls, preallocated GPU command intervals, resident managed tasks and calls including explicit transfers. The harness rebuilds optimized runtime code, without sanitizers or Metal validation. There is no language JIT or first-dispatch timing in the measured samples.

On the tested M2 Max, 256-square matmul takes 2.05 ms on CPU, 8.03 ms as a resident GPU task and 8.52 ms including transfers. The million-element reduction takes 1.54 ms on CPU, 0.72 ms resident and 1.38 ms including upload, with overlapping individual CPU/transfer samples. This supports prioritizing GPU matmul device execution and studying reduction reuse/batching; it does not establish a general placement threshold or identify a specific instruction-level bottleneck. Source supervisor continuations and application-level concurrency remain unmeasured. Parameter-layout work GPU-ABI-001 remains open.

## Strict normalization optimization

Strict GPU float32 arithmetic now normalizes significands with five straight-line integer stages (16, 8, 4, 2 and 1 bits), replacing the fixed 23-step loops for multiplication inputs and the 26-step loop after addition. Each stage shifts only a nonzero significand whose leading bit cannot overshoot the target and whose exponent has enough room above the subnormal floor. Greedy descending shifts therefore produce the same significand/exponent as repeated one-bit normalization, including cancellation and subnormal results. Zero retains its exponent until the existing signed-zero packing rules apply.

The shared MLIR lowering serves elementwise kernels, reductions and matmul. It still emits integer arithmetic through SPIR-V and Metal; it does not introduce hardware float arithmetic, FMA, reassociation or a relaxed numeric policy. Matmul's one-cell-per-lane geometry, private root stack, accepted reduction tree, ABI, task lifecycle and memory budgets are unchanged. Scalar proof artifacts reject normalization loops as well as floating arithmetic.

The APFloat corpus now adds every subnormal leading-bit position, both signs, scaling across the normal/subnormal boundary and all finite exponent fields at each cancellation distance. Historical evidence files retain their original corpus sizes; the new qualification and matched performance measurements are recorded separately.

All **119 native tests pass** after this change. The scalar corpus now contains **595,106 pairs**: each CPU variant makes 1,785,318 arithmetic comparisons and each Metal variant makes 1,823,436 including prefix/tail repetitions, with zero mismatches. The four matmul variants retain their 676-case CPU/GPU/APFloat checks and lifecycle gates; the four reduction variants retain their 570-case checks. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-normalization-2026-10-15.json) records the run and artifact hashes.

In the [alternating matched benchmark](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-normalization.md), 256-square matmul's GPU command falls from 7.73 to 2.50 ms, the resident task from 8.11 to 2.83 ms, and the transfer path from 8.67 to 3.24 ms. The CPU call remains faster at 2.21 ms. The million-element reduction's command falls from 0.234 to 0.106 ms; its resident task falls from 0.721 to 0.568 ms. Task/transfer variability still matters. Cooperative/tiled matmul and reduction batching/reuse are subsequent experiments; no automatic placement threshold is introduced.

## GPU matmul tiling study

The [isolated layout study](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-tiling.md) implements shared-memory 8×8 tiles and register 2×2/2×1 tiles through the same strict arithmetic pipeline. All four layouts, including the scalar control, pass 2,485 APFloat cases per optimized/sanitized host build, covering 161,857 cells and 4,735,946 products per layout with zero mismatches. Partial tiles, odd contraction tails, signed zeros, subnormals and nonfinite values are included.

Three interleaved processes show a 1.27× GPU-command improvement for the 2×2 tile at 512-square and about 1.11× for the two-row tile at 256-square and the tested wide/tall shapes. Both lose on smaller/narrow shapes; the shared candidate loses every measured shape. That initial study retained the scalar production kernel/runtime. The subsequent integration below selects qualified register entries within a bounded profile. The initial measurements are command intervals, not managed-task or application speedups. Next: qualify shape-aware selection over a broader shape sweep and measure any resulting integration through the managed lifecycle; eight timing shapes do not establish a reliable threshold. No numeric policy or GPU-ABI-001 limit changes.

## Initial shape-selected register matmul

The source compiler packages three independently lowered/validated entries into each matmul library: the scalar control, a two-row tile and a 2×2 tile. Each retains the reflected four-buffer ABI and `[M,K,N]` parameters. The runtime checks each selected entry's complete interface and 64-thread capability before publishing a kernel bundle. Pipeline owners remain in the existing kernel/frame ownership chain until command acknowledgement. The managed tensor/staging budgets and task result/transfer rules are unchanged; the bundle contains additional pipeline metadata.

The current performance profile applies only to the measured **Apple M2 Max**. It selects register tiles when `M >= 2`, `128 <= K <= 1025`, and `65536 <= M*N <= 263169`. Inside that envelope, a quad tile requires `K >= 256`, both output axes at least 128, and at least 262144 output cells; other admitted profile shapes use two-row tiles. Singleton rows, short/deeper contractions, smaller/larger outputs and other Apple GPUs retain scalar GPU execution. These are conservative performance choices, not language/shape restrictions or CPU fallback. The upper envelope includes the measured odd 513-square boundary and may expand with more evidence.

Register dispatch counts logical tiles, using widened arithmetic and ceiling division for partial rows/columns. The shader guards all actual reads/writes and keeps each output cell's original product/tree order. Scalar output, zero-axis behavior, explicit residency and cancellation/deadline semantics are unchanged. The shared-memory prototype remains excluded from production selection.

The [49-shape sweep](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-crossover.md) selects register candidates in 33 measured cases, all with improved command medians, while retaining scalar in 16. The [matched managed comparison and confirmation](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-15-selection.md) show selected gains reaching resident and transfer-inclusive calls, with substantial variability in unchanged controls. Both complete runs are retained. This does not establish a CPU/GPU placement threshold or improve cold pipeline startup by claim.

All 119 native tests pass. The four matmul variants now compare 678 cases, 347,871 cells and 79,315,558 products against CPU/APFloat, including managed odd-shaped row/quad dispatch. All three layouts sweep eight allocation/encoder checkpoints and cancellation/deadline/failure/core-teardown scenarios; bundle preparation sweeps four checkpoints. [Evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-selection-2026-10-15.json) records sources, artifacts, original scalar equivalence and lifecycle results. The temporary GPU parameter-layout limit remains tracked as GPU-ABI-001.

## Large strict hardware float32 matrices

Hardware-policy bundles additionally contain native float32 shared-memory kernels, compiled without fast math or contraction. On the qualified Apple M2 Max, a 16×16 output tile uses 256 lanes, two 4 KiB threadgroup panels and seven explicit scalar subtree roots per lane. It rounds each product separately and preserves the same adjacent-pair additions, including the final fold for non-power-of-two block counts. This is ordinary hardware multiplication/addition, without fused matrix instructions or reassociation.

The specialization requires `M >= 16`, `N >= 16`, at least 65,536 output cells, and `512 <= K <= 4096` with `K` divisible by 64. The compiled pipeline must support at least 256 threads per group. The upper contraction bound justifies the seven-root storage: at most 64 aligned 64-leaf roots. This bounds an optimization, not the language. Larger/odd contractions, small/narrow outputs, other devices and older bundles retain their existing GPU kernels. Software-policy bundles and other numeric types keep their previous lowering.

This qualified hardware entry takes priority over the older quad-layout profile, which otherwise masks the faster hardware path for some 512² matrices. Partial output tiles keep every lane in both barriers while guarding reads and writes. Kernel owners, checked allocation, byte budgets, cancellation/deadlines and acknowledgement cleanup retain the common runtime paths. The existing matrix/post-map/reduction batching remains in effect.

[The complete pipeline comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/large-matrix-pipeline-2026-10-25.md) distinguishes isolated screening from end-to-end timings. New conformance checks all output cells against independent APFloat references across contraction/admission boundaries, non-dyadic inputs, signed zero, subnormals, NaNs and cancellation-sensitive magnitudes. The four build modes also sweep allocation failures and hold completion while cancellation, deadlines and device failure occur.

## Float32 matrix epilogue fusion

Eligible hardware float32 matrix tasks now apply their elementwise result arithmetic inside the selected matrix kernel. For example, `relu((matmul(a,b)+bias)*scale)` writes only the adjusted matrix; a following `reduceSum` consumes that output in the same command submission. This removes one full output allocation and one dispatch. It preserves the matrix tree, separately rounded post operations, NaN/zero selection and complete-shape capture checks. No implicit bias broadcasting is added.

The compiler marks matrix post-maps and lifts only the pinned SPIRV-Cross canonical, straight-line, same-cell float32 MSL into an optional helper. The transformation validates the buffer structs, indexing and single output store, and declines unsupported control/indexing or sources over 16 KiB. At most 27 physical tensor reads fit the fused four-extra-binding interface within Metal's 31 direct buffer slots. These are optimization bounds: larger signatures, Tier 2 bindings and unsupported generated forms still execute the original post-map. The original entry remains packaged for compatibility.

Runtime selection preserves the existing matrix shape/device policy and validates each optional fused pipeline's complete reflected interface. Graphs, pre-mapped inputs, numeric programs, scalar epilogues, software arithmetic and other numeric types keep the staged path. Older bundles without optional entries also remain supported. Empty output axes retain the no-dispatch path; an empty contraction with a nonempty output still applies the post-map to positive-zero matrix values.

The task retains the post-kernel, captured tensors and independent parameter storage until GPU acknowledgement. Cancellation/deadlines suppress publication and wait for submitted access to end. Both preparation and execution allocation failures unwind through the common checked paths. [Complete-pipeline timing and qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/epilogue-pipeline-2026-10-25.md) compare the frozen preceding runtime against the fused runtime using identical shaders, driver and task archive.

[Subsequent encoder profiling and 29 microkernel experiments](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/microkernel-investigation-2026-10-25.md) place almost all sampled compute-stage time at 2048² in the matrix/fused-epilogue encoder. Larger/ping-pong panels, multiple outputs, unrolling, fixed contractions and barrier-free SIMD input sharing do not establish a broadly qualified replacement. The current production kernels and safety/arithmetic policy remain unchanged; all rejected trials and paired confirmation are retained.

## Rank-two GPU elementwise helpers

Dynamic rank-two float32 helpers now support the same strict `+` and `*`, scalar lifting, immutable local bindings and transitive helper calls as vector helpers. The original function definition still works on CPU. Scalars may appear between tensor parameters; submission owns the buffer and scalar captures.

```kalandra
fn scale(values: Tensor[float32, ?, ?], factor: float32) -> Tensor[float32, ?, ?]:
  values * factor

fn adjust(values: Tensor[float32, ?, ?], factor: float32, bias: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  scale(values, factor) + bias
```

The [resident matrix pipeline](../../native/metal-source/matrix-pipeline.k) explicitly uploads its inputs, then executes matrix multiplication, elementwise adjustment and reduction as separate GPU tasks:

```kalandra
product = try await spawn project(a, b) on gpu
adjusted = try await spawn adjust(product, 2, c) on gpu
try await spawn total(adjusted) on gpu
```

Both tensor intermediates stay on the selected device. Only the final four-byte scalar is read back. This example composes separate placed calls. Single helpers now support [elementwise input/result expressions around one reduction](native-gpu-post-reduction.md) or [elementwise expressions after one matmul](native-gpu-post-matmul.md); combining additional independent primitives inside one helper remains unsupported. The complete example produces `336` (`43a80000` through the diagnostic executable).

Every tensor operand must have the same rank and every axis must match. A `[2, 3]` matrix cannot combine with `[3, 2]`, even though both contain six elements. Empty `[2, 0]` and `[3, 0]` matrices also differ. Known mismatches are compilation errors; dynamic row and column requirements become supervisor guards, and runtime admission independently checks full shapes before submission. Scalar lifting is supported; matrix broadcasting, reshaping and implicit transfers are not. Output tasks preserve both dimensions, including zero extents, through awaits, downloads and CPU calls. Inputs may alias; results have fresh immutable storage.

The arithmetic shader remains a flat lane kernel. Equivalent vector and matrix helpers produce byte-identical raw MLIR, strict lowered MLIR and SPIR-V for all four tested entry signatures. Both the two-tensor/no-scalar binding layout and signatures with owned scalar captures are covered. The older private `compute(BinaryOperation, ...)` proof interface remains rank-one; matrix support belongs to the source-generated `elementwise` interface.

[Matrix conformance](../../native/metal-source/MatrixElementwiseChecks.h) compares compiled CPU output and actual GPU output against APFloat in **348 cases and 2,914,432 lane comparisons per build variant**, covering IEEE edge, stratified and random encodings, odd axes, workgroup tails, empty dimensions, aliased operands and complete corpus chunks. Identity calls preserve exact NaN payloads and signed zero. Results retain rank-two shapes, input buffers are unchanged, and queued captures survive caller-array mutation. The shared lifecycle harness sweeps source invocation and Metal preparation failures, cached observations, group retirement, cancellation/deadline acknowledgement, failed completion and budget failure. Debug, optimized, host ASan/UBSan and host TSan configurations execute on the Apple M2 Max with Metal API validation; host sanitizers do not instrument GPU or driver execution.

[CLI checks](../../native/metal-source/VerifyMatrixElementwise.cmake) exercise the resident pipeline with 1/2/4 workers in optimized, ASan/UBSan and TSan configurations, inspect strict artifacts and absence of tensor downloads, and run relocated bundles using only the executable and three root Metal libraries. Static/dynamic axis mismatches and hidden transfers fail explicitly. [Qualification evidence](../../native/metal-proof/evidence/gpu-matrix-elementwise-2026-10-16.json) records counts, hashes and regression results. No new throughput claim is made for this feature. Dynamic GPU parameter extents, the supported arithmetic subset, general placement/task IR and GPU-ABI-001 remain tracked limitations; subsequent capture/binding work removes the earlier two-tensor/eight-scalar storage caps.

## Serial reduction dispatch batching

The runtime now prepares all output/count buffers first, then encodes every adjacent-pair level into one explicitly serial Metal compute encoder. Pipeline state is set once, bindings change per dispatch, and Metal's serial dispatch ordering preserves the tree and all accesses to the two reused output buffers. There is no concurrent reduction of dependent levels. Fallible storage preparation finishes before the encoder opens, so allocation failures cannot leave an active encoder behind. A singleton still uses only a four-byte blit; empty input submits no command.

A million-element sum now creates one compute encoder instead of twenty. Tree dispatch count, strict shader arithmetic, two-buffer scratch capacity, per-level immutable count words, scalar readback, command admission and cancellation acknowledgement are unchanged. The 65-element lifecycle probe now sweeps 25 preparation checkpoints per rank instead of 31, while asserting the same 232 temporary bytes and exact budget boundary.

Both the serial version and a concurrent-encoder version with explicit buffer barriers pass the 653-case CPU/GPU/APFloat corpus in four build variants. The [timing comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-16-reduction-batching.md) preserves both experiments. The serial version is selected for simpler ordering and fewer encoder creations; measured warm latency is roughly flat, with mixed small improvements and regressions. No throughput gain is claimed. The direct GPU benchmark control is unchanged and does not measure managed encoder batching. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-reduction-batching-2026-10-16.json) records numerical, lifecycle and full regression results.

### Packed reduction parameters

Apple-family GPUs now store the tree's immutable count words in one shared buffer, binding a distinct four-byte-aligned offset for each dispatch. The buffer stays owned by the invocation until command acknowledgement. All words are written before encoding begins; a later level never overwrites an earlier dispatch's parameters. Other GPU families retain separate offset-zero count buffers. The capability check uses `supportsFamily(Apple1)`, independently of the shader's reflected four-byte alignment: [Metal's binding rules](https://developer.apple.com/documentation/metal/mtlcomputecommandencoder/setbuffer%28_%3Aoffset%3Aindex%3A%29) make constant-buffer offset alignment hardware-dependent, and [Apple's feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) specify four-byte alignment for Apple families.

On Apple M2 Max, a 65-element reduction now creates four temporary Metal buffers instead of ten: two private outputs, one parameter buffer and one scalar readback. Its 232-byte temporary payload and exact budget boundary are unchanged. A million-element reduction needs four temporary buffers instead of twenty-three. Empty/singleton paths still create no count buffer. This reduces buffer-object creation; it does not reduce tensor payload bytes, tree dispatches or arithmetic work.

The four reduction variants retain their 653-case CPU/GPU/APFloat corpus and now sweep 13 preparation checkpoints per rank. Cancellation, deadlines, failed completion, concurrent invocations and pending-core destruction retain their acknowledgement and cleanup gates. See the [matched timing report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-gpu-2026-10-16-reduction-parameters.md) and [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-reduction-parameters-2026-10-16.json). General GPU parameter layouts, source composition and GPU-ABI-001 remain separate work.

## Strict tensor subtraction and scalar negation

CPU and GPU helpers now support elementwise `-` alongside `+` and `*` for dense rank-one/rank-two float32 tensors. Both tensor operands must have equal ranks and every axis equal; a scalar may occur on either side. Scalar negation also works in reachable GPU helpers, including parameters and negative literals:

```kalandra
fn difference(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left - right

fn negative(value: float32) -> float32:
  -value

fn centered(values: Tensor[float32, ?], mean: float32) -> Tensor[float32, ?]:
  values + negative(mean)
```

Subtraction uses the strict integer binary32 addition path after flipping the right operand's sign. It preserves gradual underflow, zero signs, infinity handling and nearest/ties-to-even rounding. Scalar negation changes only the sign bit, preserving all other bits including NaN payloads. It does not use subtraction from zero. There is no hardware float arithmetic, FMA, reassociation, relaxed precision or implicit transfer. Whole-tensor unary `-` is now supported by the subsequent TensorUnary lowering below; this milestone introduced tensor subtraction.

The same typed operation lowers through the existing CPU tensor loop and GPU lane kernel, with source evaluation order, output shapes, owned captures, byte/command budgets and cancellation acknowledgement unchanged. The private three-operation `compute(BinaryOperation)` proof interface is unchanged. GPU-ABI-001 remains open.

The [extended strict scalar proof](strict-gpu-float32.md#source-subtraction-and-scalar-negation) qualifies subtraction and exact-bit negation against APFloat and CPU hardware. The [source fixture](../../native/metal-source/differences.k) and [source conformance](../../native/metal-source/DifferenceChecks.h) exercise tensor/tensor subtraction, scalar lifting in either order, transitive scalar negation, aliases, IEEE edges, empty/odd shapes and the full reference corpus in both ranks. Debug, optimized, host ASan/UBSan and host TSan builds retain the existing lifecycle failure sweeps. [CLI checks](../../native/metal-source/VerifyDifferences.cmake) run with 1/2/4 workers, reject static/dynamic shape mismatches and relocate the eight-library bundle. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-differences-2026-10-16.json) records counts and artifacts. No performance claim is made for this capability expansion.

## Signature-sized GPU captures

The general elementwise GPU entry path now derives its tensor/scalar layout from the checked signature. The [source fixture](../../native/metal-source/arguments.k) uses three tensors and twelve interleaved scalar parameters for vectors and matrices. A separate entry exercises 29 tensor inputs. Scalar-only entries and unsupported element types are still outside this tensor subset.

Submission builds immutable, owned tensor/scalar arrays before returning; it retains every tensor owner and copies float32 bits. Caller spans can be cleared immediately. Capture preparation failures become structured task faults, with partial arrays released before publishing the faulted task. Captures stay pinned through command acknowledgement, including cancellation, deadlines and core destruction. Successful nonempty commands release capture arrays before publishing the cached result. Empty calls and pre-commit dispatch failures retain their captures until task/frame retirement; they still obey the shared capture-byte budget. The original private two-input proof overloads remain available.

`Options.maxArgumentBytes` separately bounds requested tensor-owner/scalar-bit array payload across the registry, defaulting to 1 MiB. `Stats.argumentBytes`, `peakArgumentBytes` and `argumentPacks` expose retained captures. This is separate from the existing tensor/staging payload budget, so a tensor is not charged again when an argument owner retains it. Array/container/control allocator overhead, driver memory and RSS are not included. Allocation-failure injection covers wrapper and array preparation checkpoints as well as the existing dispatch path. Zero-byte capture budgets reject these general submissions explicitly.

The current Metal backend uses direct buffer bindings: tensor inputs, output, then a uint32 count/scalar array. [Apple's capability tables](https://developer.apple.com/metal/capabilities/) specify 31 direct buffer entries per kernel, including on Apple and Mac families. Reserving two leaves 29 physically bound inputs in this layout. This is a diagnosed backend-layout boundary, not a language-wide parameter limit. More than 29 contributing tensor inputs now select the Tier 2 argument-buffer layout below; unused logical tensors do not consume input bindings. Scalars share one device buffer; there is no eight-scalar storage cap. Counts and byte arithmetic are checked, with capture budgets, device buffer length and compiler resource limits still applying.

Generated `manifest.txt` now records each elementwise entry's tensor/scalar counts and source parameter type order. The generated host call and shader bindings derive from the same checked function. Runtime reflection checks every binding, element type, array extent, offset, stride, alignment and parameter byte length against that signature. Scalar evaluation order and packed scalar order are preserved. The compact binding projection below removes unused tensors from shader inputs while retaining logical captures and operation-derived shape guards. GPU-ABI-001 stays open for broader types and tensor operations.

[Conformance](../../native/metal-source/ArgumentChecks.h) compares vector/matrix three-tensor/twelve-scalar CPU and GPU calls with APFloat, including IEEE edges, empty/odd shapes, aliases and mutated caller capture arrays. The 29-input boundary checks numerical results, all explicit capture/dispatch failures, exact capture-budget admission, cancellation/deadlines, concurrent invocation isolation and pending-core destruction. [CLI checks](../../native/metal-source/VerifyArguments.cmake) cover 1/2/4 workers, optimized and host sanitizer builds, manifest/layout agreement, rank-independent lane arithmetic, relocation and third-argument shape/residency faults. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-arguments-2026-10-16.json) records counts and hashes. No throughput claim is made for this layout expansion.

### Unused tensor arguments and shape obligations

Elementwise GPU functions may now accept tensor parameters that do not contribute to their returned value. For example:

```kalandra
fn choose(ignored: Tensor[float32, ?, ?], values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values
```

The output has the shape and exact bits of `values`. The ignored matrix can have unrelated dimensions, including an empty dimension. Its type/rank and explicit transfer requirements still apply at the source call. The runtime owns every logical argument until command acknowledgement; it does not silently transfer or drop unused owners early.

The compiler expands supported pure helpers into lane SSA, validates every operation, then follows the returned value's dependencies to select tensor loads and instructions. It emits a sorted logical-to-physical input map, records it as `kernelN.tensor_bindings` in the manifest, and uses the same map when loading and submitting the kernel. Runtime loading copies and validates the map, checks reflection against the compact physical layout, and rejects empty, duplicate, unordered, out-of-range or oversized maps. The result shape and dispatch count come from the first contributing tensor, not the first logical parameter. An empty ignored tensor is never bound or read.

A 40-tensor logical signature with only parameter 39 returned uses one input binding, plus output and parameters. This projection does not itself change the direct Metal binding table, enable scalar-only GPU entries, or introduce new tensor operations. The subsequent Tier 2 layout below handles larger contributing-input sets. Logical captures retain the existing byte budget and checked representation limits. The two-buffer proof overload cannot submit a projected kernel.

Shape obligations come from the common typed function IR, independently of shader liveness. For example, if a discarded `ignored + other` expression precedes returned `values`, the two matrix operands must still have identical axes, even though neither supplies result lanes. Known mismatches fail compilation; unresolved obligations become supervisor guards before kernel dispatch, including when `values` is empty. Obligations propagate through helper calls. Unsupported discarded operations still fail GPU admission; this is not a way to hide unsupported code. The runtime also verifies a common complete shape for every bound lane input, ensuring buffer bounds safety. The private trusted-kernel API checks bindings and memory bounds; generated source calls additionally enforce semantic shape obligations.

The [fixture](../../native/metal-source/unused-inputs.k) combines mixed ranks, transitive unused parameters, a discarded matrix computation, noncontiguous input bindings, mixed scalar ordering and the 40-tensor signature. [Runtime conformance](../../native/metal-source/UnusedInputChecks.h) checks **30 cases and 2,380,720 exact identity lanes per build variant**, including signaling NaN payloads, signed zeros, empty/nonempty ignored tensors, vector and matrix result domains, caller-owned metadata changes, invalid maps, bounds/residency failures, and 40-argument capture retention through cancellation, deadlines and failed completion. The common source harness sweeps CPU and Metal allocation failures and cached result lifetimes. [CLI checks](../../native/metal-source/VerifyUnusedInputs.cmake) run optimized and host sanitizer builds with 1/2/4 scheduler workers, inspect strict artifacts, and check static/dynamic discarded shape failures, empty results and transactional diagnostics. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-unused-inputs-2026-10-16.json) records counts and hashes. Host sanitizers do not instrument GPU instructions. No speedup claim is made.

### Tier 2 argument buffers for large GPU calls

Elementwise GPU entries with more than 29 contributing float32 tensors now use a Metal argument buffer. Calls within the direct layout retain their existing shader ABI. Selection follows the contributing input map, so a large logical signature with only one used tensor stays on the direct path. No new source syntax is required.

The compiler emits a private `kalandra_elementwise_v3` entry and records `binding_layout=argument_buffer_tier2` in its manifest. SPIRV-Cross groups the validated SPIR-V descriptor set into one MSL device-address-space argument buffer. It contains pointers to each contributing input, the output, and the existing count/scalar block. Tensor payloads remain in their original device buffers; there is no flattening, concatenation, implicit transfer or extra tensor copy. The [SPIRV-Cross mapping](https://github.com/KhronosGroup/SPIRV-Cross/blob/main/README.md#msl-20) maps descriptor bindings to argument-buffer member IDs.

Loading requires `MTLArgumentBuffersTier2`, checks the single root binding, nested member IDs, pointer/pointee types, byte sizes, array extents, offsets and alignments, and asks Metal for the encoded byte length. Incompatible artifacts fail before kernel publication. The runtime copies the input map into owned metadata. Fixed direct-layout arrays remain only for the small direct table; indirect maps are signature-sized vectors.

Each submission creates its own argument encoder and shared resource buffer. The encoder destination is never shared between concurrent invocations of the same kernel. Resource preparation precedes opening a compute encoder, so allocation faults cannot leave an encoder open. Every indirect input is declared with `useResource(..., Read)`, output with `Write`, and scalar/count storage with `Read`, following [Apple's resource-residency requirements](https://developer.apple.com/documentation/metal/managing-groups-of-resources-with-argument-buffers). All logical owners, including unused tensors, stay retained through command acknowledgement. Cancellation and deadlines prevent publication and wait for safe cleanup.

The resource buffer's requested encoded bytes share `Options.maxBytes` with tensors and other staging buffers. Logical tensor/scalar captures retain their separate `maxArgumentBytes` budget. Empty results still validate all shape/residency obligations but need no dispatch or resource buffer. On the qualified M2 Max, 40 input pointers plus output and parameter pointers use 336 encoded bytes with eight-byte alignment. This is an observed encoded layout, not a universal size promised by the language; the runtime uses Metal's reported length.

The fixed input count is replaced by checked representation, compiler work limits, device buffer lengths and memory budgets. Unsupported devices report `Unavailable`; there is no CPU fallback. This milestone supports the existing dense rank-one/rank-two float32 arithmetic subset, not arbitrary element types or multidomain operations. GPU-ABI-001 remains open for those extensions.

The [source fixture](../../native/metal-source/indirect-arguments.k) uses 40 contributing tensors, an unused tensor of another rank, and interleaved scalar parameters in both ranks. [Conformance](../../native/metal-source/IndirectArgumentChecks.h) checks **25 cases and 793,916 CPU/GPU/APFloat lanes per rank per build variant**, including the full 595,106-pair corpus, scalar IEEE edges, aliases, empty axes and workgroup tails. It also sweeps capture/dispatch allocation checkpoints, checks exact device/capture budget boundaries, holds resources through cancellation/deadlines/failed completion, destroys a pending core, and runs two simultaneous calls through one kernel while cancelling one. The source harness additionally sweeps 87 CPU allocation failures with 1/2/4 workers.

[CLI checks](../../native/metal-source/VerifyIndirectArguments.cmake) run optimized, ASan/UBSan and TSan host programs with 1/2/4 workers, verify one root Metal binding and rank-independent raw/strict/SPIR-V lane arithmetic, run a relocated executable plus its two libraries, and reject static/dynamic shape mismatches in the last contributing tensor. The former 30-input rejection now builds and runs with an argument buffer. [Evidence](../../native/metal-proof/evidence/gpu-indirect-arguments-2026-10-16.json) records source/artifact hashes and regression results. Host sanitizers do not instrument GPU instructions. No throughput improvement is claimed; large calls add resource-buffer allocation and argument encoding.

### Whole-tensor unary arithmetic

Completed float32 tensors now support unary `-` in ordinary CPU functions and placed rank-one/rank-two GPU helpers:

```kalandra
fn negative(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  -values

fn restored(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  -negative(+values)
```

Negation preserves the rank and every extent and flips only bit 31 of each element. Positive/negative zero, subnormals, infinities, quiet/signaling NaN payloads and all other non-sign bits are preserved exactly. Double negation restores the original bits. Unary `+` remains an identity. These operations do not use subtraction from zero or multiplication by -1, which would have different zero/NaN behavior.

The checker emits a distinct TensorUnary node for tensor negation, carrying the operand's shape without inventing equality obligations. It still rejects pending or fallible task values: `-work` does not imply await. The CPU backend emits ranked loops with `arith.negf`; allocation bounds, immutable inputs, cleanup and cooperative task cancellation use the existing tensor machinery. Task extraction retains and remaps the unary operand. The GPU backend expands the same checked operation into lane SSA and applies the existing strict integer sign-bit lowering. Unsupported operations, element types and GPU ranks still fail admission; the mixed supervisor's general expression limits remain unchanged.

Negation composes with supported arithmetic and transitive helpers. A function returning `-(left + right)` retains the addition's complete-axis shape requirements. That addition follows the existing arithmetic NaN policy; the subsequent negation flips the result's sign. Exact payload preservation applies to negation and identities, not to arbitrary preceding arithmetic. There is no implicit device transfer, arithmetic reassociation, CPU fallback or numeric-policy relaxation.

The [source fixture](../../native/metal-source/tensor-negation.k) runs negation, nested double negation, unary plus, and a negated sum in both ranks, then combines downloaded and CPU-produced tensors. [Conformance](../../native/metal-source/TensorNegationChecks.h) checks **68 cases and 3,177,168 CPU/GPU lanes per rank per build variant**, including **2,382,876 exact-bit comparisons** for the unary-only paths. It covers the full 595,106-pair corpus, aliases, zero axes, workgroup tails, unchanged inputs and propagated result shapes. Composed addition/negation is compared independently with APFloat, using NaN classification for the arithmetic result.

Debug, optimized, host ASan/UBSan and host TSan variants retain the common source harness's allocation-failure, cached ownership, byte-budget, cancellation, deadline and failed-completion checks. [CLI qualification](../../native/metal-source/VerifyTensorNegation.cmake) runs with 1/2/4 workers, verifies rank-independent raw/strict/SPIR-V artifacts and relocated eight-library bundles, tests static/dynamic composed shape faults, rejects unary arithmetic on a pending task, and checks an empty outer matrix axis. It also runs ordinary CPU tasks with inline tensor negation and rank-three CPU lowering in optimized and host sanitizer builds. [Evidence](../../native/metal-proof/evidence/gpu-tensor-negation-2026-10-16.json) records sources, artifacts and regression results. Host sanitizers do not instrument GPU instructions. No performance claim is made.

## Strict tensor division

Dense rank-one/rank-two float32 GPU helpers now accept tensor/tensor division and scalar operands on either side. The same TensorBinary operation works in ranked CPU functions and CPU task extraction. Tensor operands require identical rank and every corresponding extent; no tensor-axis broadcasting or implicit transfer is introduced.

```kalandra
fn quotient(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left / right

fn reciprocal(value: float32) -> float32:
  1 / value

fn scale(values: Tensor[float32, ?, ?], divisor: float32) -> Tensor[float32, ?, ?]:
  values / divisor
```

Reachable scalar helpers can divide too. This does not admit arbitrary scalar-returning GPU entry points; their existing placement restrictions remain. The [strict integer lowering](strict-gpu-float32.md#strict-division) preserves nearest/ties-to-even rounding, gradual underflow, signed zero, infinity and canonical arithmetic NaN. Source evaluation order is preserved: division is not replaced with a separately rounded reciprocal and multiplication. Shape faults remain checked task failures, while floating division by zero follows IEEE result rules.

The [fixture](../../native/metal-source/division.k) and [conformance harness](../../native/metal-source/DivisionChecks.h) cover tensor/tensor division, both scalar orders, and multiplication through a transitive reciprocal helper. Each rank and build variant checks **436 cases and 3,499,236 CPU/GPU/APFloat lanes**, including the existing pair corpus and 98,304 additional directed division pairs, aliases, empty axes, odd tails, immutable inputs and cached ownership. Debug, optimized, host ASan/UBSan and host TSan variants retain common cancellation, deadlines, byte budgets, failed completion and allocation-failure checks; nine CPU allocation failures are unwound with each of 1/2/4 workers.

[CLI checks](../../native/metal-source/VerifyDivision.cmake) verify strict artifacts, identical vector/matrix lane code, relocated eight-library bundles, complete-axis shape faults, CPU task division, and a GPU regression distinguishing `5 / 3` from `5 * (1 / 3)`. Host sanitizers do not instrument GPU instructions. [Qualification evidence](../../native/metal-proof/evidence/gpu-division-2026-10-16.json) records the sources, artifacts and test results. No performance claim is made.

## Inline CPU expressions in mixed programs

The mixed program's `try parallel` body now accepts ordinary float32 arithmetic, unary signs, `reduceSum`, and `matmul` directly. Small CPU operations no longer require named wrapper functions:

```kalandra
host = try await transfer(resident, devices.cpu())
projected = matmul(host, weights)
reduceSum((projected - 1) / 2)
```

These expressions execute on the CPU. An awaited GPU tensor remains on the device until explicitly downloaded; `resident + 1` does not select a GPU kernel or insert a transfer. Pending tasks and device targets are not numeric operands. GPU placement continues to use an explicit named `spawn ... on gpu` call.

The supported host subset is float32 scalar arithmetic and rank-one/rank-two float32 tensors, with `+`, `-`, `*`, `/`, and unary `+`/`-`. Scalars lift on either side of tensor arithmetic; two tensor operands need equal rank and every corresponding extent. `reduceSum` uses the existing row-major balanced tree; `matmul` remains rank two with separately rounded products and that fixed tree. Division retains IEEE zero/nonfinite behavior and does not become reciprocal multiplication. Remainder and implicit numeric conversions remain unsupported.

Nested expressions evaluate their operands in source order and may appear in bindings, final results, ordinary function arguments, transfer arguments, or scalar arguments to a placed GPU call. Supported inline results retain shape facts derived from known input axes, so a later known mismatch can still fail compilation. Unknown dimensions retain the common typed operation's preconditions and fail before CPU descriptor export or arithmetic. Rank/axis mismatches are not accepted merely because element counts match.

The compiler checks a private operation wrapper through the common native frontend and imports its typed nodes with fresh function, symbol, value, and dynamic-extent identities. Only compiler-owned operation/type spellings enter the wrapper. User source identifiers are not interpolated into generated IR. The compiler caches wrappers by operation and operand ranks; declarations and source diagnostics remain separate from user function names. Imported nodes carry the original expression location, and inline invocation faults report the call site. Nested argument checking may grow these arenas, so the call bridge does not retain invalidatable references across that work.

The existing checked CPU ABI handles allocation, descriptor copying, immutable result ownership, cancellation polling and cleanup. This step does not remove CPU argument copies or fuse chains of tensor expressions. Placement, residency and continuation lowering still use the target-specific supervisor; CPU spawn is now supported by the subsequent increment below; a shared placement-aware task IR, richer control flow, and broader types remain subsequent work.

The [source fixture](../../native/metal-source/host-expressions.k) combines inline arithmetic, GPU placement, explicit downloads, matmul, reduction, nested arguments and an internal-name collision. [Runtime checks](../../native/metal-source/HostExpressionTests.cpp) run debug, optimized, host ASan/UBSan and host TSan builds with 1/2/4 workers, including **34 CPU allocation failures per worker configuration**, **41 Metal preparation failures**, cached observations, byte-budget failure, cancellation, group deadlines and failed completion. Every path retires scheduler and runtime resources. Host sanitizers do not instrument GPU instructions.

[CLI checks](../../native/metal-source/VerifyHostExpressions.cmake) cover those build modes, vector and empty-matrix expressions, exact signed-zero/NaN/rounding outputs, deterministic generation, executable/library relocation, and transactional rejection of missing downloads, pending tasks, targets, wrong arity, remainder, and known rank/axis faults. A dynamic matmul mismatch checks the imported shape obligation with 1/2/4 workers. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/host-expressions-2026-10-16.json) records sources, artifacts and regression results. No performance improvement is claimed.

## CPU tasks in mixed programs

A mixed program can now submit ordinary CPU functions to the same bounded scheduler as GPU calls. An unplaced `spawn` selects CPU execution; `on devices.cpu()` makes that choice explicit. Device-target aliases retain their meaning.

```kalandra
cpu = devices.cpu()
projection = spawn project(input, weights)
summary = spawn total(input) on cpu

projected = try await projection
summed = try await summary
```

Arguments and the optional device selector evaluate once, in source order, before admission. The compiler generates a concrete capture/frame layout for each CPU call site. Submission synchronously retains immutable host Buffers and copies float32 scalar values before making the child runnable. Repeated references to the same source value share one capture field; aliasing backing storage remains valid. The packet's stack lifetime does not limit the child's captures, and tensor payloads are not copied at admission. Existing scheduler record, frame-byte, queue and worker limits bound admission.

The worker uses the existing checked CPU invocation ABI. Tensor arguments are copied into child-owned CPU descriptors when execution begins, compiled loops retain cancellation polling, and immutable tensor results acquire a Buffer lease before the descriptor owner is released. Scalar and tensor task results have distinct result layouts. Cached observations keep their results alive after child/frame cleanup and scheduler retirement. Cancellation is checked before execution and after the compiled call; cancelled execution never imports or publishes an early-return output descriptor. Captures and temporary storage retire through the existing task settlement protocol.

A CPU task can consume host tensors or scalars, including explicit GPU downloads and scalar GPU reduction results. A CPU-produced tensor can be uploaded for a later GPU call. Awaiting a tensor task does not change its device residency. CPU tasks reject device-resident arguments unless explicitly downloaded; no automatic transfer or fallback is added.

CPU-only functions may retain fixed tensor parameter/result dimensions. GPU admission now walks only the placed function's checked helper closure. Ordinary CPU expressions used to compute GPU arguments do not become GPU helper bodies. Fixed dimensions in an actual GPU closure still fail the current GPU capability check. Static CPU argument shape faults fail compilation; unknown shape requirements are checked in the child before its arithmetic. Child faults cancel and settle siblings through the existing parallel-group behavior.

The [source fixture](../../native/metal-source/cpu-spawn.k) submits independent CPU tensor/scalar tasks, runs GPU work, feeds downloaded data into another CPU task, and uploads a CPU result for a subsequent GPU call. Its final matrix is `[[52, 54], [64, 66]]`. [Runtime checks](../../native/metal-source/CpuSpawnTests.cpp) run with 1/2/4 workers in debug, optimized, host ASan/UBSan and host TSan builds. They sweep six parent invocation allocation failures and, independently, four allocation failures for a tensor child and two each for dynamic/fixed scalar-reduction children. They also check task-core admission allocation failures, every nominal Metal preparation failure, byte budgets, GPU completion cancellation/deadlines/failure, cached result ownership and resource retirement.

Direct generated-entry tests destroy submitting packets and source invocations before execution, mutate scalar packets after admission, reuse aliased inputs, preserve results beyond scheduler destruction, and reject a malformed fixed-shape child. A controlled two-worker test runs actual compiled CPU entries concurrently and cancels one independent group while preserving the other group's result. [CLI checks](../../native/metal-source/VerifyCpuSpawn.cmake) cover default/explicit/aliased CPU placement, zero-argument and twelve-scalar calls, vectors, empty matrices, GPU scalar results, deterministic generation, relocation, missing downloads, unobserved children, invalid targets/arity, static shape rejection and dynamic child shape faults. [Evidence](../../native/metal-proof/evidence/cpu-spawn-2026-10-16.json) records sources, artifacts and regression results. Host sanitizers do not instrument GPU instructions. No speedup is claimed.

This increment supports pure ordinary float32 scalar/rank-one/rank-two CPU functions as children of mixed-program parallel blocks; lexical nesting is supported by the subsequent increment below. Nested task creation inside those functions, broader values, richer control flow, and shared placement-aware task IR remain separate work; scoped recovery is covered below. The CPU-only compiler's placement handling is unchanged. A `--target metal` program still requires an actual GPU call; CPU-only programs use the ordinary CPU target. The compiled host keeps its runtime provider alive until every child and pending GPU completion has settled.

## Nested parallel scopes in mixed programs

Mixed-program `main` now accepts lexically nested `try parallel` expressions. Each block opens a child group of its enclosing source group and joins it before continuing. CPU calls, GPU calls and transfers belong to the innermost group. Await and join suspend the supervisor continuation, leaving its worker available for other work; they do not recursively drain the scheduler or block a worker thread.

```kalandra
resident = try parallel:
  host = try parallel:
    try await spawn shift(values, 1)
  input = try await transfer(host, gpu)
  try await spawn scale(input, 2) on gpu

host = try await transfer(resident, devices.cpu())
```

A block returns a completed float32 scalar or rank-one/rank-two tensor. Returning a tensor preserves its residency and ownership: the GPU value above remains on the GPU after the inner group closes. Downloading it is a separate explicit operation. An annotation on the surrounding binding supplies type/shape context for a final tensor literal inside the block. No implicit transfer, promotion or CPU fallback is introduced.

Bindings are lexical. Inner bindings may shadow outer names, duplicate bindings within a block are rejected, and inner names disappear when the block finishes. Every task admitted in a block must be observed before that block closes; tasks and device selectors cannot escape as block results. An outer task may be observed inside an inner block owned by the same supervisor, and that observation satisfies the outer obligation. Completed tensors keep their existing immutable Buffer lease through group settlement.

The existing task quotas apply to nested groups as well as tasks. Depth is measured from the runtime root, including the outer source parallel group. Group-control records remain allocated until their owning supervisor retires, so sequential scopes also count toward its configured group-storage budget. Exceeding a quota fails and settles the invocation; this increment does not introduce unbounded group storage or reclaim group records early.

The [source fixture](../../native/metal-source/nested-parallel.k) combines three levels of groups, inner shadowing, CPU children, a resident GPU result, a later explicit download, and observation of an outer task inside an inner scope. Its result is `48` (`42400000` float32 bits). [Runtime checks](../../native/metal-source/NestedParallelTests.cpp) cover 1/2/4 workers, cached observations, four parent CPU allocation failures per worker configuration, all 31 Metal preparation failure positions, depth/group/ready-queue limits, byte budgets, and cancellation/deadline/failure while nested GPU compute is pending. Scheduler and runtime resources return to zero after retirement. [CLI checks](../../native/metal-source/VerifyNestedParallel.cmake) add contextual literals, empty tensors, tensor results, deterministic generation, relocation, rejected task/selector/local escapes and unobserved work, and a dynamically failing CPU child in an inner group. Debug, optimized, host ASan/UBSan and separate host TSan variants are covered; GPU instructions are not sanitizer-instrumented. [Qualification evidence](../../native/metal-proof/evidence/nested-parallel-2026-10-16.json) records the regression and artifacts.

This step adds lexical blocks in the mixed supervisor. Task creation inside ordinary CPU helper bodies and a shared placement-aware task/residency IR remain separate work; scoped `catch`/recovery follows below. An actual GPU placement is still required by this target. Distributed execution remains subject to its separate design review.

## Scoped TaskError recovery in mixed programs

Mixed `main` now supports completed expected-error results from `parallel` and `await`, `catch fn(error): ...`, exhaustive matching over `TaskError` and `FaultKind`, and handler-local `try`/`fail`. Results may be bound for later handling. Numeric operations, transfers, arguments and final results reject an unhandled expected error; discarding one is also diagnosed.

```kalandra
outcome = parallel:
  work = spawn project(input, weights)
  try await work

projected = outcome catch fn(error):
  match error:
    TaskError.Fault {kind: cause, message: detail}:
      match cause:
        FaultKind.Shape: fallback
        _: fallback
    TaskError.Cancelled: fallback
    TaskError.DeadlineExceeded: fallback
    TaskError.AdmissionRejected {reason: explanation}: fallback
```

`try` inside a parallel block exits through that block's close/join continuation when a child fails. The handler runs only after every descendant and external GPU completion in the failed group has settled. Successful blocks skip their handlers entirely. A completed tensor keeps its immutable backing lease through the branch; no tensor payload copy or automatic host/device transfer is introduced by `catch`.

A failed child still fails its containing group. Catching an individual `await` cannot heal that group; catch the completed `parallel` result to continue in a healthy enclosing scope. A direct CPU call, literal allocation, shape refinement, or kernel-loading fault in the supervisor remains an execution fault. Ordinary `catch` does not clear that invocation's fault or turn it into a value. External cancellation of the owning invocation also remains terminal. Faults in a supervised child are instead exposed through the settled group's `TaskError.Fault`.

Handler and match results need a common float32 scalar or rank-one/rank-two tensor type, with the same residency and GPU index. Different inferred tensor extents join as dynamic dimensions; explicit surrounding annotations still constrain both paths. Shape checks after recovery validate dynamic dimensions before downstream work or final return. `TaskError.Fault` exposes `kind` and `message`; `AdmissionRejected` exposes `reason`. The current fault mapping follows the CPU target: shape code 1, allocation code 2, and Native for other execution faults. The message/reason fields are typed diagnostic text; general string operations are outside this target's current subset.

Handler-local `try` and `fail error` return an expected error from that handler. A later `catch` may handle it, or a surrounding `try` may propagate it. `fail` needs a concrete numeric success-type context, inferred from the caught result when no explicit binding annotation is supplied. Propagation preserves the TaskError variant and source location. The runtime's compiler-facing `Task::fail(Failure)` validates the variant and gives an existing invocation fault or cancellation precedence; it never resets the invocation's status.

Bindings introduced by handlers and match patterns stay local. Match cases are checked for known families, valid fields, duplicate/unreachable cases, and exhaustiveness. Tasks or expected errors created on a conditional path must be handled on that path. Handling an ancestor obligation only inside a fallback does not discharge it on the success path. Task handles, error objects and device selectors cannot become numeric branch results.

Another worker may fail and settle a group between evaluating an argument and admitting the next child. Generated admission checks now inspect the failed group when admission returns an empty handle, preserving its original error and exiting through the group join. A failed group is not reported as an unrelated invalid handle. Runtime quota and admission rules are unchanged.

The [source fixture](../../native/metal-source/recovery.k) runs GPU work in a protected group, observes a CPU child shape failure, selects a typed fallback, and continues with GPU work in its healthy parent. It produces `180` (`43340000` float32 bits). [Runtime checks](../../native/metal-source/RecoveryTests.cpp) cover 1/2/4 workers, ten supervisor CPU allocation failures per worker configuration, all 53 nominal Metal preparation failure positions (19 recover, 34 stop safely), cached observations, failed GPU completion recovery, scope cancellation/deadline recovery, terminal owner cancellation, budgets, and zero-resource retirement. Scope cancellation yields once before resuming admissions, forcing the already-settled-group path without a timing assumption. [Task-core checks](../../native/tests/TaskTests.cpp) verify propagation of all four error variants and their locations, invalid tags, and precedence of an existing invocation fault.

[CLI checks](../../native/metal-source/VerifyRecovery.cmake) cover four build variants, lazy handlers, rethrows, successful/failing handler-local `try`, deferred fallible handler results, tensor fallbacks, dynamic post-recovery shapes, deterministic generation, and relocation. Rejection cases cover type/residency mismatch, invalid/non-exhaustive patterns, leaked bindings, unhandled errors and conditional task/error obligations. Host ASan/UBSan and TSan do not instrument GPU instructions. [Qualification evidence](../../native/metal-proof/evidence/recovery-2026-10-16.json) records the regression and artifacts. No performance improvement is claimed.

At this milestone, the mixed target required a single outer `try parallel` in `main` and an actual GPU placement. Subsequent shared-IR and resumable-helper work below removes the main-only coordination restriction. Broader value types remain separate work. Distributed execution retains its design-review gate.

## Shared placement frontend

The [shared typed placement IR](placement-ir.md) now validates complete mixed programs before artifacts are published. Device selectors, transfer tasks, placed invocations and tensor residence are visible in `placement.ir`, with the existing shape and error-control structures. This is a semantic integration step; the current target-specific C++ supervisor emitter still walks source syntax. Zero outer dimensions now use the accepted explicit shape spelling, such as `tensor([], [0, 3])`; an annotation does not change the rank of `tensor([])`. Numerical lowering and runtime ownership are unchanged.

## IR-driven supervisor continuations

Mixed-program supervisor expressions now lower from the shared placement-aware typed IR, including resolved symbols, checked calls, shape guards, nested scopes and TaskError recovery. The source expression walk and duplicate pattern-field resolution have been removed. The [placement IR design](placement-ir.md#ir-driven-supervisor-continuations) describes evaluation order, path-local value reuse, qualification and the remaining backend boundary.

## Common continuation planning

CPU task lowering and the Metal supervisor now consume the same checked slot/group/branch plan. Scope settlement, recovery exits and selector/argument order are planned once; target emitters retain their numerical dispatch and value layouts. The [continuation design](continuations.md) describes the two call policies, inspection artifacts, conformance and remaining mixed helper scope.

## Resumable mixed helper bodies

[Direct resumable helpers](continuations.md#resumable-mixed-helper-bodies) can now own parallel scopes, submit CPU/GPU work, suspend for transfers/tasks, recover settled errors and return completed host values. Task coordination can live in transitive helper functions, including a nonfallible wrapper that catches a fallible helper's errors. Main no longer needs a single outer parallel block when its callees own their work. Pure numeric calls retain their checked CPU ABI and GPU kernel eligibility. An actual GPU placement is still required by the Metal target.

Resumable helpers currently expand into their caller's continuation. [Independently spawned helpers](continuations.md#independently-scheduled-mixed-helpers) now use separate child frames and the existing scheduler ownership/cancellation rules. Resident tensors, selectors and task handles retain their existing escape restrictions. The continuation design records examples, limits and lifecycle qualification.

## Local modules and projects

[Apple Metal function modules](native-metal-modules.md) share the native module loader, visibility and nominal source identity with CPU builds. Numerical helpers and mixed CPU/GPU coordinators can be imported, aliased and re-exported, using relative paths, explicit offline package mappings or native project manifests. Module composition preserves existing float32/rank/placement limits and explicit transfers; it does not make managed host values GPU-compatible.

## Fixed GPU tensor signatures

[Fixed and dynamic tensor extents](native-gpu-fixed-shapes.md) now share the existing numerical kernels. Parameter annotations, checked local bindings and result annotations preserve their shape obligations through transitive numerical helpers and module imports. Unknown extents are checked before kernel admission, including unused parameters; known mismatches remain compile-time diagnostics.

## Reduction and matrix forwarding helpers

[GPU numerical forwarding](native-gpu-forwarding.md) now follows local aliases, named/imported helper calls and completed result forwarding around one supported reduction or matrix operation. Buffer mapping preserves reordered and repeated operands. It reuses the existing shader/runtime interfaces and preserves fixed/dynamic shape checks; general extra primitives and recursive forwarding remain diagnostic.

[Elementwise expressions before a reduction](native-gpu-pre-reduction.md) now
compose strict float32 arithmetic and one reduceSum inside a numerical GPU helper.
Two GPU stages share one scheduled task and retain the intermediate on device.
[Post-reduction scalar arithmetic](native-gpu-post-reduction.md) now runs once
on the GPU before final scalar readback. [Elementwise matrix results](native-gpu-post-matmul.md)
now compose tensor/scalar arithmetic after one multiplication using resident
stages. [Matrix input arithmetic](native-gpu-pre-matmul.md) now adds independent
resident maps of either/both operands. [One matrix consumed by one reduction](native-gpu-matmul-reduction.md)
can now share a scalar task; further primitives remain separate work.

## Planned GPU interoperability

The [advanced GPU interoperability follow-up](../roadmap.md#advanced-gpu-interoperability--planned-follow-up) tracks two deferred workstreams: application-supplied compute shaders/kernels and adapters for external GPU compute libraries. Custom kernels provide specialized computation; library adapters reuse existing optimized operations. These are separate interfaces and qualification efforts, outside the current release gates. Custom shader loading would use the managed GPU task runtime. Apple Metal comes first. Public loading syntax, artifact formats and binding declarations are not designed yet. Existing private kernel loaders are not a public foreign-shader API, and reflection does not validate arbitrary shader behavior.

## Elementwise matrix inputs

[Matrix input arithmetic](native-gpu-pre-matmul.md) now composes independent
float32 elementwise maps of either/both operands with one multiplication and an
optional post stage. One tensor task retains GPU intermediates and original
immutable captures. Each map has its own projected direct/Tier 2 binding layout;
strict multiplication shaders, shape contracts, memory/in-flight budgets and
cancellation acknowledgement remain unchanged. Source/CPU/GPU/APFloat,
four-mode lifecycle and installed relocation qualification are recorded in the
linked design. Additional primitives and broader GPU language/type integration
remain N2 work.

## Matrix multiplication consumed by reduction

[Matrix-to-reduction composition](native-gpu-matmul-reduction.md) now lets one
float32 helper return `reduceSum(matmul(input, weights))`, optionally transforming
either/both matrix inputs first. One scalar task retains the resident matrix
and reads back only its final scalar. Matrix/reduction shaders, strict trees,
shape guards, budgets and command acknowledgement rules remain unchanged.
[Arithmetic between the product and reduction](native-gpu-matmul-adjust-reduction.md)
now also stays resident. [Scalar arithmetic after this pair](native-gpu-matmul-scalar-reduction.md) now
also stays on GPU. Broader numerical graphs remain N2 work.

## Matrix-result arithmetic before reduction

[Matrix adjustment/reduction composition](native-gpu-matmul-adjust-reduction.md)
now supports `reduceSum((matmul(input, weights) + bias) * scale)`, including
optional input maps. One scalar task owns the full resident sequence and keeps
the strict multiplication and final reduction trees unchanged. Full-shape
guards, direct/Tier 2 bindings and failure/ownership rules reuse existing paths;
only the final scalar is read back. [Final scalar arithmetic](native-gpu-matmul-scalar-reduction.md)
now also stays on GPU; broader graphs remain separate N2 work.

## Final scalar arithmetic after matrix reduction

[The final scalar epilogue](native-gpu-matmul-scalar-reduction.md) now supports
expressions such as `reduceSum(matmul(input, weights) + bias) / count` within one
GPU task. Matrix and scalar captures remain independent. The scalar dispatch
runs in the final reduction command before the only readback, including empty
sums and IEEE division by zero. Existing shape, budget and acknowledgement rules
remain unchanged; broader numerical graphs and GPU types remain N2 work.

## Matrix product graphs

[Product-only graphs](native-gpu-matrix-graphs.md) now keep several matrix
products under one tensor task, including helper calls and shared intermediates.
The runtime releases retained operands after their last use and reuses the strict
matrix kernel without reassociation or implicit host transfers. Combining several
products with arithmetic or reductions remains N2 work.

## Reduction after a product graph

[Matrix graph reduction](native-gpu-graph-reduction.md) now consumes a chain or
shared-intermediate graph with one final `reduceSum`, followed by optional scalar
arithmetic under the same task. All matrices stay resident, and only the final
scalar is read back. Empty results still execute scalar arithmetic on the
positive-zero sum. Tensor arithmetic between products remains future work.

## Arithmetic within matrix graphs

[Mixed matrix/arithmetic graphs](native-gpu-graph-arithmetic.md) now support
float32 tensor operations before, between and after products, including scalar
lifting, shared intermediates and one optional final reduction. One task owns the
resident sequence, each arithmetic shader binds only its live tensor operands,
and full shapes are checked before dispatch. Adjacent-stage fusion remains
optimization work.

## Multiple reductions

[Multiple resident reductions](native-gpu-multiple-reductions.md) now support
rank-one/rank-two float32 inputs and shared intermediate matrices under one
scalar task. A device expression combines the reduced scalars before the only
readback. Empty inputs seed positive zero; larger expressions use the existing
Tier 2 binding path. [Resident reduction feedback](native-gpu-reduction-feedback.md) now feeds later tensor arithmetic and matrix products inside scalar-returning graphs. [Tensor results after feedback](native-gpu-tensor-feedback.md) now remain resident too, including single-reduction normalization.

## Scalar choices and comparisons

[Pure scalar `if` expressions and float32 comparisons](native-gpu-scalar-control.md)
now compose with tensor helpers and resident reduction graphs. Comparisons and
selection use bit encodings to preserve IEEE behavior. Tensor-producing and
dispatching branches remain outside this bounded subset.

## Input-layout experiments

The [weight-packing investigation](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/weight-packing-investigation-2026-10-25.md) measures GPU construction, repeated immutable weights, activation layouts and explicit scratch storage. Some square cases improve, but rectangular and interior-shape regressions reject both the broad and narrower policies. Packing remains experimental benchmark code; production kernels, strict arithmetic, budgets and acknowledgement ownership are unchanged.

## Grouped output-tile traversal

The [memory-access follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/memory-access-pipeline-2026-10-26.md) adds an optional hardware-float32 entry that visits output tiles in groups of 16 tile rows. On the qualified M2 Max profile, the host selects it for M,N between 512 and 65,535, at least 1M output cells and 16 MiB combined input storage, within the existing K=512…4096/multiple-of-64 register envelope. These are performance limits, not language limits. Other shapes and older bundles retain their original entries.

The grouped entry shares the original strict arithmetic helper and keeps each output cell's producer, balanced tree and fused epilogue unchanged. It adds no copy, dispatch or per-invocation buffer. Two extra optional plain/fused pipelines affect cold preparation. Complete 2048² pipeline runs improve resident medians by about 4.9% and upload-inclusive medians by 3.2–4.5%; the report retains small-input latency regressions and qualification limits. All 44 selected numerical/runtime gates pass.
