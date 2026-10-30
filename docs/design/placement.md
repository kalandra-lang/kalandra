# Kalandra placement, transfers, and bulk tasks proposal

Accepted placement direction — 2026-10-15. This extends the accepted [structured tasks](execution.md), [tensor types](tensors.md), [numeric rules](numbers.md), and [automatic memory model](memory.md). These rules describe the fresh language, not implemented features of Kalandra 0.13.

The goal is to run the same supported calculation on a selected executor without hiding its data movement. Placement selects execution. Transfer moves bulk data. Neither changes the mathematical type of a value.

## Accepted decisions

- Add an optional `on target` clause to a spawned invocation.
- An omitted target means the local CPU executor; ordinary calls remain in their current execution context.
- Use library target selectors: `devices.cpu()`, `devices.gpu()`, `devices.gpu(index)`, and `devices.remote(name)`.
- Move tensors explicitly with `transfer(value, target)`, an asynchronous operation owned by a parallel group.
- Tensor results remain where computed until explicitly transferred. Small scalar results are included in the task outcome.
- Use a bounded `parallelMap` library helper for collections; ordinary map remains sequential.
- Unsupported placement fails visibly. There is no implicit CPU fallback, precision change, retry, or arbitrary code upload.

Selectors, transfer, and parallelMap are selected standard-library APIs with compiler-visible contracts. They do not introduce general user-written generics or expose allocator/lifetime parameters.

## Place the invocation

```kalandra
fn total(values: [float]) -> float ! TaskError:
  try parallel:
    work = spawn sum(values) on devices.cpu()
    try await work
```

This is the explicit spelling of local CPU spawn. CPU means an executor in this application on the current host, not an external service. A nested spawn without `on` also selects local CPU; it does not inherit a remote deployment or a GPU queue accidentally. An ordinary call inside a CPU task executes there. An ordinary supported call inside a GPU computation belongs to that device computation.

The clause attaches to the whole invocation. `try await spawn sum(values) on devices.cpu()` groups as `try (await (spawn sum(values) on devices.cpu()))`. It remains valid only inside an owning parallel group.

Evaluate the call target, arguments, and placement expression once, in source order, before submission. Awaiting later does not defer argument evaluation or task admission. Target selection cannot change argument evaluation effects.

A selector identifies an executor; it does not open it or allocate a tensor. Static target mistakes are diagnostics. Missing devices, unavailable deployments, and runtime capability mismatches are typed task errors when work or transfer is submitted. Device indices are host configuration, not portable hardware identities.

## Default and specific GPUs

```kalandra
work = spawn project(input, weights) on devices.gpu()
```

Devices.gpu() selects the configured default GPU. With multiple GPUs, select a specific device using a zero-based index:

```kalandra
work = spawn project(input, weights) on devices.gpu(1)
```

Index 1 selects the second GPU exposed to this application. The no-argument selector resolves to a stable configured device for the application execution; it does not load-balance each call or silently switch devices. If no default is configured, use exposed device 0. Missing or unsupported devices fail under the placement error rules; there is no implicit CPU fallback.

Both calls require input and weights to be available on the selected device. A named binding is equivalent and useful when several transfers and invocations share a target:

```kalandra
gpu = devices.gpu(1)
work = spawn project(input, weights) on gpu
```

These spawn examples appear inside a parallel group. A bare undeclared gpu is not a built-in target name. The selector is an ordinary immutable value; direct calls and named bindings use the same placement rules.

## Transfer once, compute there, return what is needed

```kalandra
fn project(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2]:
  matmul(samples, weights)

fn projectOnGpu(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    inputCopy = transfer(samples, gpu)
    weightCopy = transfer(weights, gpu)
    deviceInput = try await inputCopy
    deviceWeights = try await weightCopy
    work = spawn project(deviceInput, deviceWeights) on gpu
    deviceResult = try await work
    try await transfer(deviceResult, devices.cpu())
```

Inputs are assumed host-resident at this entry point. Both uploads are submitted before either is awaited. Computation starts after both complete. The final download is visible in the source. The completed host tensor can leave the group; transfer/task handles cannot.

Transfer returns an owned task handle, not a tensor disguised as a pending value. Awaiting it produces the same logical tensor type `! TaskError` with storage available at the destination. Transfer to an already available destination may complete without copying, but retains task/error/ownership behavior. Sources remain valid and unchanged.

For repeated work, retain immutable weights on the GPU and reuse them across calls. A returned device tensor can safely outlive the group that computed it: its managed storage is independent of a live child task. Automatic lifetime management releases storage only after all readers and commands have completed. No allocation scope or manual free is introduced.

## Residency without device parameters in every type

`Tensor[float32, ?, 2]` remains a mathematical type. Managed storage also carries residency metadata. The compiler tracks known locations through bindings and calls; a location unknown until runtime is checked against the chosen executor before dispatch. This is placement validation, not an implicit transfer.

A host tensor used directly as a GPU argument is rejected when known, or fails admission when its residency is dynamic. The diagnostic points to the missing transfer. A host-side tensor operation, including scalar indexing, cannot silently download a device buffer. Transfer to CPU first. Constants and scalar arguments are part of the invocation payload; tensor literals created inside a GPU computation are device values and do not represent an upload of a captured host tensor.

Inside a placed function, tensor parameters must be available on that executor. Tensor outputs and intermediates belong to that execution context. Pure helper calls follow the same context; they do not bounce back to CPU. The compiler may share physical storage on unified-memory systems, or optimize redundant transfers, while preserving explicit synchronization, validation, and value lifetimes.

Initially transfer accepts a dense tensor. Arbitrary records containing mixed-location tensors, resource capabilities, and recursively distributed object graphs are deferred. Ordinary scalar/record/array arguments may cross CPU/remote invocation boundaries under concrete bounded contracts; GPU parameters initially allow tensors and supported scalars only. Bulk numerical arrays should be tensors when transfer control matters.

Small scalar outputs are copied into the completed task outcome. Thus a GPU reduction returning float32 can be awaited without a separate scalar-download operation. This is a documented completion cost, not permission to materialize an arbitrary tensor during await. Awaiting a tensor result establishes completion and returns a managed reference; it does not download its elements.

## Which functions can run on GPU?

Placement is explicit, but admission is checked. Initial GPU functions are supported pure tensor/scalar computations with known types, shapes, and a backend-supported control-flow subset. Unsupported operations are diagnosed. Files, network requests, spawning tasks, arbitrary native calls, and unbounded host callbacks are not device operations.

Do not add `@gpu` or require authors to maintain separate copies merely to choose placement. The compiler checks the selected function and its transitive callees for that target. Dynamically selected device identity is permitted; an unknown arbitrary callable is not automatically device-compilable. A CPU supervisor coordinates GPU and remote tasks.

Float64 stays float64. A device without the required format or strict numerical behavior cannot silently run a float32/relaxed kernel. The accepted fixed reduction tree and rounding policy apply. A valid backend implementation or an explicit unsupported-target error is required. A separate relaxed numerical policy remains future work.

## Remote execution is deployment, not code teleportation

```kalandra
fn remoteProject(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2] ! TaskError:
  try parallel:
    executor = devices.remote("analysis")
    inputCopy = transfer(samples, executor)
    weightCopy = transfer(weights, executor)
    remoteInput = try await inputCopy
    remoteWeights = try await weightCopy
    work = spawn project(remoteInput, remoteWeights) on executor
    remoteResult = try await work
    try await transfer(remoteResult, devices.cpu())
```

The deployment named analysis must already contain a compatible compiled entry for project. Build tooling generates its identity, signature, numeric/shape requirements, and error contract. Host configuration supplies endpoint and credentials. No token belongs in source code or a generated public contract.

A remote target initially identifies a deployment's CPU executor. It does not implicitly choose a remote GPU; heterogeneous remote-device selection follows validated local GPU and remote CPU execution. A remote CPU task may coordinate its own explicitly configured device work.

Transfer validates schemas, dimensions, size limits, and authorization before publishing a complete immutable value. The remote runtime owns retained data through bounded authenticated leases/references; a local managed proxy is not a raw server pointer. Normal release frees unused remote data; leases reclaim abandoned data after client/process loss. Expiration/disconnection produces typed unavailability, never memory-unsafe access. The concrete lease protocol must be specified before remote storage is implemented.

Task handles stay inside their group. Remote data references may outlive it, but are deployment-bound managed values, not ordinary serializable identifiers that another client can forge or reuse. Quotas and lease policy are explicit deployment configuration. Partial uploads are reclaimed on failure.

Captured data requires the same explicit placement/contract checks as arguments. Scoped resources, local task handles, arbitrary closures, and raw pointers cannot be smuggled across a boundary. Initial remotely placed entries are statically known functions with deployable dependencies; dynamic closure shipment is outside this proposal.

Calling a separately authored service from JS, Python, or another language is a different boundary: generated request/response contracts, not this language's task/storage handles. Keep that interface independent of the Kalandra runtime. Wire format, transport, external service import syntax, and version negotiation require a later contracts proposal; this document does not select Sia or promise language-specific runtime SDKs.

## Bounded collections without another keyword

```kalandra
fn totals(batches: [[float]]) -> [float] ! TaskError:
  try parallelMap(batches, sum, ParallelOptions {
    target: devices.cpu(),
    maxInFlight: 4,
  })
```

ParallelMap is a structured library operation: it owns a group, invokes the callback once per input element on a successful complete run, observes all admitted results, and returns completed values in input order. No task collection escapes. It can be called without an enclosing parallel block because it creates its own group. Callback input/result/error types are concrete and inferred from the call; its generic library signature will be formalized with generics.

MaxInFlight must be positive. It caps admitted, unfinished invocations, not just physical threads; runtime-wide worker, queue, memory, and device limits still apply. The helper stops producing submissions when full and resumes after completion. Nested helpers share root limits and must progress with one CPU worker. Backpressure must not park every worker while descendants wait in its queue.

ParallelOptions has two required fields initially; no optional/default-field syntax is implied. GPU/remote targets obey the same supported-function, data-residency, and deployment rules. The helper does not upload every batch or replicate captured weights automatically. Tensor batch preparation and a streaming producer API follow separately.

On failure, stop admission, cancel unfinished children, and settle them before returning. Expected callback errors are preserved alongside TaskError. Ordering the output does not order effects. There are no implicit retries or exactly-once network guarantees. Only the successful complete path promises every element was processed; failed runs may leave unsubmitted elements and already-performed external effects.

The in-flight bound does not bound the retained input array or final output array. This first helper is for finite collections. Infinite streams, incremental output, byte-based budgets, and dataset tiling need their own APIs rather than misleading claims about constant memory.

## Errors and cleanup

Extend the accepted TaskError family with typed placement outcomes before implementation:

- UnsupportedTarget: the required function, dtype, or numerical behavior is unavailable.
- Unavailable: the selected executor or retained remote data cannot be accessed.
- TransferFailed: data movement failed before a valid result was published.
- ContractMismatch: deployment entry/schema/revision differs from the trusted contract.

Exact payload fields and distinctions from authentication/device-loss errors belong to the contracts/runtime API proposal. These cases are accepted in direction; their complete declarations remain pending in variants.md. Application errors remain their original error families; do not turn every failed remote invocation into an untyped message.

Shape, residency, capability, and contract checks happen before the affected operation executes where possible. A device disappearing after admission is still possible and must produce a settled failure. Cancellation does not free a buffer still referenced by an active kernel, DMA operation, or remote task. Deadline expiry requests cancellation; release follows confirmed completion or a documented isolation/reset mechanism. Remote response loss does not prove an effect never occurred.

## Validation before implementation

1. Parse `on` as part of spawn; verify inline try/await grouping and once-only evaluation order.
2. Default spawn selects local CPU; ordinary helper calls retain the current execution context.
3. Overlap independent transfers, then dispatch only with validated input residency and shapes.
4. Reject hidden host/device copies, including host indexing of device tensors.
5. Permit completed tensor results to outlive a group while rejecting escaping task handles.
6. Enforce strict numerical capabilities; never silently narrow precision or fall back targets.
7. Validate deployed entry identity, schemas, credentials, quotas, and lease expiration without uploading arbitrary code.
8. Reclaim partial transfers and retain in-use buffers through cancellation/device failure.
9. Preserve typed application and execution errors across local, device, and remote tasks.
10. Bound parallelMap admission, preserve output order, stop on failure, and prove nested single-worker progress.

## Accepted direction and next step

The project owner accepted `spawn ... on target`, explicit tensor transfer, residency checks, deployment-bound remote execution, and bounded parallelMap, including default/indexed GPU selectors. Exact runtime payloads, callable residency contracts, and wire/deployment APIs still need specification before implementation.

Review the [complete CPU/GPU/remote application example](compute-example.md), including its data movement, errors, and cleanup. Then scope the first CPU compiler milestone before implementing GPU or remote backends.

## Deferred: direct execution and memory controls (LL-001)

Track lower-level access as a later design area, after the safe compiler/runtime foundation is established. These are exploration goals, not accepted syntax or implemented capabilities:

- Construction of updated immutable values and broader collection/view operations, distinct from exposing raw pointers. [Read-only CPU numeric array indexing](array-indexing.md) and [tensor scalar indexing](tensor-indexing.md) are now implemented; these do not expose raw memory access.
- Explicit vector/SIMD operations for specialized CPU algorithms, alongside automatic compiler vectorization.
- User-defined GPU kernels with controlled lane/workgroup access, bounds rules and data ownership.
- Finer coordination primitives such as channels and barriers, with clear ordering, cancellation and deadlock behavior.
- Specialized GPU memory/synchronization controls where measured workloads justify them.

Keep ordinary functions, immutable values and structured tasks as the default experience. Direct access must have explicit safety and placement contracts; adding it does not authorize unrestricted shared writes or raw-pointer arithmetic. Any unsafe escape hatch needs a separate use case and design review. Preserve room for these features in compiler/runtime architecture, without fixing an API before those requirements are understood.
