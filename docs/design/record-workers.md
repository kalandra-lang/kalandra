# Records in CPU workers

Native CPU workers can accept and return the [immutable CPU records](records.md) supported by the native compiler. Records retain their nominal declaration identity through nested tasks, helper calls, completed groups, and repeated awaits.

```kalandra
type Point:
  x: int
  y: int

fn shift(point: Point, distance: int) -> Point:
  point with { x: point.x + distance }

fn main() -> int ! TaskError:
  original = Point { x: 2, y: 3 }
  moved = try parallel:
    work = spawn shift(original, 10)
    try await work
  moved.x + original.x
```

The result is `14`. The child receives an immutable snapshot; `original.x` remains `2`. The completed record remains valid after its group settles. A repeated `await` reads the same cached outcome. Updating an observed value cannot change that cached result.

## Types and completed values

Record fields can use the ten implemented CPU numeric scalar types, including [8/16-bit fields](narrow-arrays.md), `Bool`, another [finite nested record](nested-records.md), or [numeric arrays and tensors](record-collections.md). A mixed record can contain different scalar types. Worker transport preserves the exact integer and floating-point field values, including signed zero and NaN payload bits; passing a record does not perform arithmetic or convert its fields.

A task handle is not a record value. Use `try await work` before accessing or updating the success value, or recover the completed `await work` result with `catch`. Task handles cannot escape their owning group. Existing observation, cancellation, deadline and admission rules apply to record tasks.

A record's identity belongs to its declaration. Two records with identical field layouts remain different types, including same-spelling declarations in different modules. Imported function results keep their original identity, and can be passed back to helpers or workers expecting that declaration. [Type-name imports/exports](type-exports.md) preserve this same nominal identity.

Records may be used alongside numeric array/tensor captures and results. Synchronous numeric map callbacks can construct and read records internally. [Anonymous callbacks can capture entire completed records](record-closures.md), including nested collection fields, directly or within CPU workers.

## Recovery

Record results use the same `TaskError` channel as numeric results. `try` propagates an expected failure, `catch` can produce a fallback of the same nominal record type, and exhaustive error matches can return records. A handler can use `fail error` when its record success type is known, or start further work and propagate its failure through the enclosing fallible function.

No success fields are published when a task fails. Recovery runs under the existing group-settlement rules. Arithmetic and bounds failures, cancellation, deadlines, admission rejection and allocation failure do not produce a partial record.

## Representation and lifetime

The shared continuation planner carries the record declaration table into its MLIR kernel module and preserves record operands in written evaluation order. MLIR's C wrappers return aggregate results through an output pointer; record arguments remain values. The CPU task emitter uses matching native struct payloads and distinct canonical layout descriptors for each nominal record type.

Records with numeric/Boolean leaves, either directly or inside nested inline records, copy the complete value without backing-storage leases or per-record heap allocations. Records containing collections instead use recursive retention of their [owned field storage](record-collections.md). Scheduler frames and task bookkeeping still use the scheduler's bounded allocation and cleanup mechanisms. Larger records consume more of that existing task-memory budget. This internal layout is not a public C/C++ library ABI or wire contract.

[Record arrays](record-arrays.md) now support retained worker captures/results, including nested numeric collection fields. General record printing and GPU/mixed-target record execution remain separate implementation gates. `main` must still return a supported numeric scalar, `Bool`, numeric array or tensor. Native distributed execution requires the agreed design discussion before implementation.

## Qualification

On Apple M2 Max, all 107 focused checks passed. Worker programs passed in debug, optimized, ASan/UBSan, and separate ThreadSanitizer builds: 96 compiled source programs and 344 trusted task-harness runs across those variants. The harnesses checked 4,880 snapshot/cache/layout/settlement cases, including 4,608 exact raw-bit success cases at one, two and four workers. Coverage includes overwriting the original capture packet before execution, surviving values after scheduler destruction, unchanged cached outcomes, guarded output storage, wrong-layout rejection, pre-dispatch cancellation/deadline, bounded admission rejection and allocation-failure injection. Generated programs cover mixed and Boolean-only records, 20-field records, nested helpers, repeated awaits, recovery/rethrow/matching, imported record results and numeric collection integration. Frontend checks reject malformed record identities and constructor task/error flags before native storage. Existing compiler, ownership, task, collection, module and selected Metal regression checks passed. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage sources remain unchanged. This is focused qualification, not a full-suite claim; the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-workers-2026-10-18.json) records scope and hashes.
