# Records captured by numeric callbacks

Anonymous numeric map callbacks can capture completed immutable CPU records, including nested records with numeric array/tensor fields and [Bool-array fields](record-bool-fields.md).

```kalandra
type Calibration:
  offset: int
  corrections: [int]

fn main() -> [int] ! TaskError:
  calibration = Calibration { offset: 10, corrections: [2] }
  try parallel:
    work = spawn map([1, 2, 3], fn(value): value + calibration.offset + calibration.corrections[0])
    first = try await work
    again = try await work
    again
```

This returns `[13, 14, 15]`. The callback reads the whole `calibration` binding directly; there is no need to extract each field before defining it. The map still executes sequentially on one worker. This does not make each element a separate task.

## Capture and lifetime rules

Only referenced outer bindings are captured. Repeated reads of one record binding capture it once. Nested anonymous callbacks carry the same lexical binding through their enclosing callbacks. A callback parameter can shadow an outer record name without capturing that record.

The callback's private function receives the record as an extra typed parameter. Its declaration identity is preserved: a different record with an identical physical layout is not interchangeable. Imported helper results keep their original identity when captured, and callbacks can pass them back to imported functions.

Synchronous callbacks borrow the enclosing invocation's immutable record value. A directly spawned map or a worker function containing a map uses the existing recursive record retention at worker boundaries. This keeps collection backing alive through nested tasks, cached results and repeated awaits. There is no new public closure representation, scheduler mechanism or runtime ownership model.

Record construction, field reads and immutable updates inside the callback follow the [existing collection-field copy baseline](record-collections.md). Updating a captured record produces another value; it cannot change the outer binding or an earlier cached worker outcome. Earlier reclamation and shared backing remain optimization work.

## Completed CPU values only

Await or recover a record-producing task before capturing its result. Task handles and unhandled fallible values remain invalid captures. Device selectors and device-resident values remain outside this callback subset. Callbacks and their reachable helpers must remain synchronous.

[Sequential maps](array-map.md#record-elements-and-results) now accept numeric or record parameters/results. [Record arrays](record-arrays.md) can also be captured and indexed. [Named CPU function references](native-function-values.md) can now be bound, passed, returned and captured by inline map callbacks. Escaping captured closures, multiline anonymous bodies and GPU records remain separate work.

## Qualification

Frontend checks cover all six CPU numeric families, record capture deduplication, lexical shadowing, transitive nested captures, nominal identity and unfinished-value rejection. Native tests cover nested collection fields, immutable updates, imported helpers, completed worker results captured by later callbacks, nested closures, direct and nested workers, empty maps, fault recovery, cancellation, deadlines, allocation failure, cached results and exact numeric identity mapping. The execution suites run in debug, optimized, ASan/UBSan and separate ThreadSanitizer variants. All 80 focused checks passed on Apple M2 Max. Across the four build variants, the new suites compiled 132 source programs, ran 1,056 worker harness invocations, and checked 3,159,552 exact lanes through 2,880 identity-map invocations. All 640 tracked numerical artifacts and 20 numerical/checked-ABI/runtime/storage source files remain unchanged. This is focused qualification, not a full-suite claim. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-closures-2026-10-18.json) records scope and hashes.
