# Application errors in local CPU tasks

A local worker can return `T ! E`. Awaiting its handle produces `T ! (E | TaskError)`: the child's declared error families remain distinct from scheduling, cancellation and execution failures. The same rule applies when E is already a union or contains TaskError.

```kalandra
error ReadError:
  Missing:
    name: String

fn readValue(name: String) -> int ! ReadError:
  if name == "missing":
    fail ReadError.Missing { name: name }
  42

fn readInWorker(name: String) -> int ! (ReadError | TaskError):
  try parallel:
    try await spawn readValue(name)

fn main() -> int:
  readInWorker("missing") catch fn(error):
    match error:
      ReadError.Missing { name }: length(name)
      TaskError.Fault { kind, message }: 0
      TaskError.Cancelled: 0
      TaskError.DeadlineExceeded: 0
      TaskError.AdmissionRejected { reason }: 0
```

`spawn` submits the call once. Repeated awaits read its cached immutable outcome, including managed error fields, without rerunning it. The result must still be handled or propagated; the success type cannot silently discard any error family. The enclosing function's channel must cover the full group result.

## Handling versus propagating

An application error is an ordinary worker outcome. Catching it inside the group does not cancel siblings. The parent can recover, continue reading other children and finish successfully.

`try await` propagates the error to the owning parallel group. A synchronous application-fallible call inside the group can similarly use `try`; an early `if ...: fail ...` guard can explicitly fail that group. These paths stop admission, request cancellation of unfinished descendants and wait for settlement before returning the original typed error. Rethrows, replacements and nested groups use the same rules.

A checked child execution fault still fails the group and cancels siblings. Catching the awaited value does not reopen a failed group or erase its recorded execution failure. Recovery belongs at the enclosing group boundary. Parent-side execution faults remain execution faults, rather than becoming application errors.

One primary group outcome is preserved. If an execution failure was already recorded when the parent propagates an application error, that earlier failure wins. Once the application error has reserved the primary outcome, cancellation used to drain siblings cannot replace it. Concurrent failures have no promised deterministic winner. Secondary diagnostic aggregation is not added by this implementation.

Unhandled application errors report their original `Family.Variant` and exit with status `89`. Unhandled TaskError results retain their task classification and source location and exit with status `88`.

## Implementation and ownership

The compiler observes the child's own result layout, then widens its channel when necessary. The widening temporary belongs to the scheduler-budgeted frame, avoiding native stack growth from large typed payloads. Success data and active error payloads remain separate. Existing immutable backing retention keeps managed fields alive across observation, suspension, nested calls and settlement; inactive variants and absent success payloads are not retained.

A propagated application error stays in the owner's typed continuation frame while the scheduler drains its group. A private scheduler operation reserves the primary outcome under the scheduler lock. Its internal cancellation marker has no application payload and is consumed by the generated group-exit path. It is never substituted for the original error exposed to the program.

This is local CPU support. GPU error transport, distributed contracts and public C/C++ interfaces belong to separate work. [Error aliases in record/enum payload fields](error-payload-fields.md) are supported as completed data. [Whole-value narrowing in match arms](error-narrowing.md) preserves those values through selected and residual family rethrows.

## Qualification

The [compiled worker cases](../../native/cmake/VerifyWorkerErrors.cmake) exercise all ten numeric widths, application unions, execution faults, cached awaits, sibling recovery, nested groups, rethrows, replacements, synchronous group propagation, early failure guards, managed String/array/record payloads and CLI classification. Allocation injection checks cleanup on successful, recovered and failed paths. [Scheduler tests](../../native/tests/TaskTests.cpp) check primary reservation, cancellation, settlement and preservation of existing fault/cancellation/deadline/admission metadata.

Debug, optimized, ASan/UBSan and TSan execution and the surrounding frontend/runtime regressions are recorded in the [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-errors-2026-10-19.json).
