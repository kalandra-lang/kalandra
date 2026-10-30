# Function-valued local CPU worker results

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Implemented native subset — 2026-10-20. A statically named CPU worker can return a proven synchronous function value, including a closure with supported immutable data and direct nested function captures. This extends [worker closure arguments](native-worker-closures.md) and [nested captures](native-worker-nested-closures.md) under the [transport contract](worker-function-transport.md).

```kalandra
fn makeAdder(offset: int) -> fn(int) -> int:
  values = [offset]
  fn(value: int): values[0] + value

fn main() -> int ! TaskError:
  transform = try parallel:
    try await spawn makeAdder(40)
  transform(2)
```

The [complete fixture](fixtures/worker-function-results.k) returns `42`. The function is called after its parallel group settles. Neither the task handle nor the producer's frame needs to remain alive. Calling the returned function is synchronous; indirect `spawn transform(...)` remains staged.

## Transfer and ownership

The producer copies its function environment into the task's cached outcome before its execution invocation settles. Each successful await copies that cached environment into the observer's invocation and retains the required immutable data backing. Later observations receive independent environment storage, with the same cached function value and captured data. Releasing the task group or scheduler does not invalidate already observed functions.

Completed returned functions can be passed onward to statically named CPU workers. Provenance follows immutable bindings, direct factories, conditional/match alternatives, try/await, parallel results and compatible catch results. Coordinating workers can return a closure obtained from a child. Function bodies themselves remain synchronous.

Existing tagged allocation leases, exact-signature target checks, recursive call admission and copied-environment budgets apply at both outcome and observation handoffs. Cached outcome environment bytes reserve the root scheduler byte quota until outcome settlement. Observer copies reserve their destination's budget when one is bound. External observer invocations retain the existing per-invocation copied-environment limit; shared leaf payloads and allocator overhead are not a global heap bound.

Failed copies leave observations unpublished and partial managed storage remains ledger-owned until settlement. Cancellation before or after outcome construction settles the task as cancelled and releases the outcome. Copy allocation failures retain checked allocation faults. A cancelled observer is not converted into an internal execution fault.

## Error channels

A factory's expected errors and the returned callback's expected errors remain distinct. For example, `-> (fn(int) -> int) ! BuildError` describes a fallible factory returning an infallible callback; `-> (fn(int) -> int ! ReadError)` describes an infallible factory returning a fallible callback.

Expected-error results copy and retain only their active error storage; they do not inspect an inactive function payload. Successful results validate and copy the function environment. Captured owned data in ordinary expected-error payloads follows existing retention rules. The [owned-error fixture](fixtures/worker-result-errors.k) constructs a callback from the error’s retained String and array after the worker group finishes. Function-containing error payloads remain rejected.

## Remaining restrictions

Returned function provenance must be proven. Forwarded function parameters and unproven indirect factories/projections remain staged, as do functions inside worker argument/result aggregates or captured aggregates. Spawned-map function captures and indirect spawn remain separate work. GPU callbacks, distributed function shipment and public C/C++ callback interfaces are unchanged.

## Qualification

The [CPU qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-function-results-2026-10-20.json) records 49 qualified selected CTest checks on Apple M2 Max with LLVM/MLIR 22.1.8. This is a focused selection, not the full project suite. Implementation and new result tests were frozen before the main run. That run passed 43 checks; four legacy checks retained the old blanket rejection of function-valued worker results. Their assertions now reject unsupported captures or unproven results instead. All six affected frontend/execution checks pass after the test-only correction, including positive block-closure worker results. No compiler or runtime implementation changed during that correction.

The new suites compile/run 88 source programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer modes. Private harnesses run 144 producer tasks and 288 cached observations, then invoke returned functions 2,304 times after releasing their groups and cores. They check failed/cancelled observations, outcome allocation failure, cancellation during outcome copying and shared root byte quotas. There are 96 additional CLI worker runs, 384 allocation-failure probes and 36 preserved artifacts after unsupported-result rejection.

The owned-error guide also compiles and runs separately in all four modes, retaining error payload data in a callback used after group completion. Frontend suites accept 48 worker-callable programs and reject ten unsupported boundaries and six forged IR cases in both ordinary and sanitized builds. Earlier closure transport, callable aggregates, modules, expected errors, scheduler and task recovery retain their selected regression coverage.
