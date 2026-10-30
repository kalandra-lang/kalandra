# Function-containing values across local CPU workers

Implemented native subset — 2026-10-20. Records, enum variants, function arrays and record arrays containing proven synchronous functions can cross local CPU worker argument and result boundaries. Closures may capture those values transitively. This extends [closure arguments](native-worker-closures.md), [nested captures](native-worker-nested-closures.md) and [function-valued results](native-worker-function-results.md).

```kalandra
type Job:
  run: fn(int) -> int
  label: String

fn apply(job: Job) -> int:
  job.run(2) + length(job.label)

fn main() -> int ! TaskError:
  offsets = [38]
  transform = fn(value: int): offsets[0] + value
  try parallel:
    try await spawn apply(Job {run: transform, label: "hi"})
```

The result is `42`. The [array-result fixture](fixtures/worker-callable-aggregates.k) also demonstrates repeated observation and use after group completion.

## Supported values and provenance

Transport traverses nested records, the active enum payload, function arrays and record arrays. Function fields can have different signatures, including expected-error callback signatures. Ordinary immutable numeric, Bool and String storage retains its existing checked handoff behavior. Empty function arrays and all ten numeric widths are supported. The [fallible-callback guide](fixtures/worker-callable-aggregate-errors.k) invokes an error-returning callback after receiving its containing record from a worker.

The compiler proves function origins through constructors and immutable updates, aliases, compatible branches/matches, direct factories, completed task/group results, and array/field projections whose input origins are proven. Slices and concatenation preserve that proof. [Direct parameter provenance](native-worker-parameters.md) now also supports forwarding and returning function-containing parameters, including captured parameters in returned closures. The analysis remains conservative across callers and indirect producers. Overwriting one field does not automatically prove an otherwise unproven base record.

Only statically named worker calls gain this support. Indirect spawn, spawned collection operations/maps with function-containing captures, and function-containing expected-error payloads remain staged. Ordinary enum values containing functions are supported; a declared expected-error channel containing functions remains rejected. GPU callbacks, distributed execution and public C/C++ callbacks are unchanged.

## Copying without source mutation

A receiver gets new typed descriptor/value storage for its function environments. Function-containing array buffers are also cloned before their cells are rewritten. Each function cell then uses the trusted exact-signature copy path, recursively retaining or copying its captures into the destination invocation. Ordinary immutable data backing can remain shared through retained ownership.

Inline records are copied to scratch storage before traversal. Enum tags are validated and only active fields are visited; inactive function slots are not interpreted. Array copies validate canonical packed descriptors, nonnegative lengths, stride, alignment and backing leases before reading cells. Function-array copy layouts distinguish complete callable signatures rather than sharing one opaque array layout.

Copy helpers register their entries before emitting recursive bodies. Array loops use fixed entry-block temporaries, avoiding per-element native stack allocation. Checked call admission bounds recursive traversal, including forged cycles through captured arrays. The existing conservative native stack checks also cover generated aggregate helpers.

A complete value is published only after all nested copies succeed. Failed copies leave the external output untouched and never rewrite the source. The destination ledger owns partially built storage until settlement. Copied function-array storage shares the existing cumulative 1 MiB invocation copy budget and root scheduler byte reservations with closure environments. These private resource guards are not a global heap bound; retained leaf payloads and allocator overhead are not fully charged by them.

Independent observations can outlive their producer, task group and scheduler. Whole-environment sharing, shared function-array storage and copy elimination remain optimization work; no pointer-identity or zero-copy guarantee is introduced.

## Qualification

The [CPU qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-callable-aggregates-2026-10-20.json) records 57 passing selected CTest checks on Apple M2 Max with LLVM/MLIR 22.1.8. This is a focused selection, not the full project suite. Implementation and test sources were frozen before the final run.

New execution suites compile/run 108 source programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer modes. Private harnesses check creator-independent lifetimes, source immutability, 48 invalid/stopped arrays, 28 allocation probes, active/inactive/invalid enum tags, known wrong signatures, invocation copy budgets and forged cyclic array captures. They run 144 producers with 288 cached observations and 576 function calls after releasing the group/core, plus 96 argument tasks after destroying their submitting contexts. Queued cancellation and shared root result quotas settle without leaked reservations.

There are 120 additional CLI worker runs and 480 allocation-failure probes. The fallible-callback guide also compiles/runs in all four modes. The worker frontend catalog accepts 64 programs, rejects five remaining unsupported boundaries and checks six forged IR cases in each frontend mode. Former rejection cases for the newly supported boundaries now run as positive regressions. Existing closure/result transport, callable aggregates, modules, typed errors, task recovery and scheduler checks retain their selected coverage.
