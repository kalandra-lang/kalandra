# Owned upload snapshots on unified-memory GPUs

An upload still creates an immutable device value. On a unified-memory Metal
device, the runtime can populate a new shared `MTLBuffer` directly from the
retained CPU source. It does not need a staging buffer, a private destination,
or a blit command to copy between those two GPU allocations.

Apple recommends shared storage for buffers populated by the CPU and read by
the GPU. CPU writes must finish before GPU access. The runtime enforces this by
publishing the device result only after copying and padding the whole snapshot.
See [Apple's storage guidance](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus)
and [shared-memory synchronization requirements](https://developer.apple.com/documentation/metal/mtlstoragemode/shared).

## Ownership and visibility

- The destination owns new storage. It never wraps the caller's pointer or the
  CPU allocation lease with `bytesNoCopy`.
- The task packet retains its source, including any offset CPU allocation lease,
  until completion or cancellation. Source inputs remain immutable while the
  upload is pending, as in the existing transfer contract.
- The frame owns the partially copied result. No consumer can access it until
  TaskCore publishes a successful result. Device buffers still reject
  `hostBytes()` even when Metal internally uses shared storage.
- All CPU writes precede any subsequent GPU command using the published value.
  No CPU writes occur after publication. Shader outputs and temporaries retain
  private storage and their existing completion acknowledgement requirements.
- Byte accounting charges the full physical destination, including word padding
  for narrow numeric types. It no longer charges a staging allocation that does
  not exist. CPU allocation leases continue to charge their full backing.

## Cancellation and scheduling

Each resume copies at most 1 MiB and yields if bytes remain. The task checks
cancellation before each chunk; TaskCore checks again when publishing the
result. Allocation and driver calls themselves remain indivisible operations,
so this is a copy-work bound, not a wall-clock deadline guarantee.

The partial source and destination survive yields through normal task ownership.
Cancellation, deadline propagation or core teardown discards the unpublished
snapshot and releases its admission and memory reservations. A CPU-only copy
needs no external GPU lease or completion callback. Cancellation cannot expose
a partially copied result. The shared path keeps the existing finite in-flight
admission budget, including while a copy is yielded.

No GPU command is submitted for the shared upload. Consequently it cannot
report a GPU command-completion error and does not consume the runtime's
`failNextCompletion` test hook. The next actual GPU command still observes that
hook. Device allocation errors and memory/admission errors remain checked.
The graph's compute cancellation checkpoints are unchanged.

## Fallback and scope

`Device::hasUnifiedMemory()` gates the path. Non-unified devices retain the
staging/private/blit implementation and its external acknowledgement contract.
The private backend `Options::sharedUploads` switch can force that fallback for
qualification and matched storage experiments. It is not a language-level
ownership or arithmetic option.

The source compiler, numeric shaders, reduction order, task runtime and CPU
backend are unchanged. This is a Metal storage and upload-lifecycle change.
It does not make GPU computation universally faster than CPU computation.
Warm resident execution must be measured separately from the upload savings.

## Qualification and fair controls

The upload suite covers private/shared policies, empty and partial-word buffers,
boundaries around 1 MiB, multi-chunk copies, one/two/four workers, cancellation
before and during copying, deadline propagation, allocation and exact memory
budgets, in-flight admission, CPU lease survival, independent snapshots,
concurrent cores, runtime/core teardown and preservation of device-failure hooks.
Existing ten-type/rank-zero-through-eight storage tests and generated GPU
programs exercise the shared inputs; the externally acknowledged transfer suite
continues to exercise the private fallback.

Benchmark controls compare the frozen previous runtime, both storage policies
in the same candidate binary, and MPSGraph with the same fresh owned shared
snapshot contract. Allocation and CPU copying are inside timed uploads for
both implementations. No output readback is timed. Independent per-cell
numerical bounds and bit-identical Kalandra outputs are checked outside timing.
The initial unbounded direct-copy prototype is benchmark-only; production
acceptance measures the bounded, yielding implementation.
