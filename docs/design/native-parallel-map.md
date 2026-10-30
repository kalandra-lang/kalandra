# Native CPU parallel map

`parallelMap` applies a callback to an immutable array through the existing CPU task scheduler and returns an array in input order. It is a compiler-recognized operation with ordinary call and pipeline syntax:

```kalandra
fn double(value:int)->int:
  value*2

fn main()->[int] ! TaskError:
  try parallelMap([1,2,3],double)
```

The pipeline spelling is `try (values -> parallelMap(double))`. The result above is `[2,4,6]`. Ordinary `map` remains sequential, and `spawn map(...)` still schedules one sequential loop in a worker.

## Evaluation, types and ownership

Evaluate the input and callback/capture expressions once in the caller, in that order, before submitting work. The callback may be a named function, an inline anonymous function, or a supported bound function value. Each mapped element must produce one completed scalar value supported by ordinary array mapping: the ten CPU numeric types, Bool, String, records/enums, or functions. The element type may change; output length and input order are preserved. This does not add arrays of tensors or arbitrary nested arrays.

An infallible callback produces `[U] ! TaskError`. A callback with application channel `E` produces `[U] ! (E | TaskError)`. Handle or propagate the result with ordinary `try`/`catch`; a tail result may be returned through a matching declared channel. Typed errors may carry the same proven callable payloads as other CPU workers.

Input elements, captures, and results use existing checked worker layouts. Function-containing values require proven origins and eligible captures, including for empty arrays. Returned environments and ordinary managed backing survive producer settlement and onward handoff. The compiler writes internal output storage during construction; no partially initialized success array is exposed.

Empty input returns an empty array without spawning element tasks. Callback expression evaluation and static capture checks still apply.

## Scheduling and errors

The initial implementation submits one task per element in **batches of up to eight**, then observes those tasks in input order. It fills one final output array and releases the settled batch's child records before proceeding. Live child records and groups do not grow with input length. The batch size is an implementation tuning choice, not a source-level array-size limit or a permanent language parameter limit.

Maps share the invocation's scheduler and existing worker pool. Awaiting children suspends a continuation rather than blocking a worker thread. A map can run inside an ordinary worker or be explicitly spawned as `spawn parallelMap(...)`. Named and inline callbacks may coordinate nested tasks/maps through their typed channels. [Proven bound CPU function values](native-resumable-callables.md) may also coordinate tasks; recursive coordinating calls now use bounded CPU continuations; unknown targets and GPU callbacks remain gated.

Existing scheduler limits still apply, including ready-queue capacity, group depth, records, in-flight work and retained bytes. Insufficient admission capacity produces `TaskError.AdmissionRejected`; bounded batching does not promise success under arbitrarily small quotas or unlimited nesting. Concurrent maps share those global limits.

On an observed application error, stop and settle unfinished children before propagating the primary group outcome. Execution faults, admission failures, cancellation and deadlines retain the task contract's existing precedence and cleanup rules. Successful result ordering does not promise a deterministic winning error when failures race. Already completed callbacks are not rolled back.

This is ordinary local CPU execution. Placement-aware/Metal callbacks, distributed execution and NVIDIA/CUDA are separate work; the compiler does not silently substitute CPU work for a requested device target.

## Implementation and optimization

The typed map operation records explicit parallel execution and the complete error channel. Continuation lowering emits bounded batches, checked element reads, ordinary child entries, awaits, output stores, joins and explicit settled-group reclamation. Numeric reads honor validated aligned/offset/strided views; owned arrays retain their checked packed representation. A private runtime operation reclaims an owned group only after it has closed, settled and been observed. Compiler-generated handles cannot escape a reclaimed batch.

Adaptive task granularity, capacity-aware admission, wider/configurable scheduling windows, straggler handling, and capture-copy reduction remain optimization work. Tiny callbacks may cost more to schedule than to execute; this milestone makes no throughput or speedup claim.

## Qualification

The source catalog covers call/pipeline syntax, empty inputs, all supported numeric widths, closure captures, callable inputs/results, records, Strings, Bool, typed errors and callable error payloads, checked faults, explicit spawn, and nested named/inline callbacks. It also covers malformed signatures, dropped error channels, unsafe empty-input captures and forged typed IR.

A private generated-entry probe verifies all 257 ordered results with a 16-record quota, measures overlapping callback execution with multiple workers, and checks one-worker progress, producer-independent result ownership, cancellation, deadlines, quota rejection and balanced cleanup. CLI probes inject allocation failures and verify caller evaluation order and preservation of existing build artifacts on rejected programs. These are correctness checks, not performance benchmarks.

**69/69 selected checks passed.** See the [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-parallel-map-2026-10-20.json) for the exact selection, test corrections, probe limits and remaining scope. This is not the full project suite.
