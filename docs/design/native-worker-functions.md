# Named function arguments to local CPU workers

Initial native subset — 2026-10-19. **Current extension:** [captured closure arguments](native-worker-closures.md) now support anonymous targets, direct factory results and immutable CPU data captures. The details below record the original capture-free increment.

Implemented native subset — 2026-10-19. A statically named CPU worker can receive a proven capture-free named synchronous function as an ordinary argument. This implements the first stage of the [local-worker function transport contract](worker-function-transport.md). Captured environments, function-valued worker results and indirect spawn remain separate implementation stages.

```kalandra
fn double(value: int) -> int:
  value * 2

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn main() -> int ! TaskError:
  transform = double
  try parallel:
    work = spawn apply(21, transform)
    try await work
```

The result is `42`. No worker-specific function type, decorator, capture list or allocator annotation is needed. The [complete fixture](fixtures/worker-named-functions.k) also checks aliases and repeated observation of the same task.

## Supported provenance and signatures

Direct named references and immutable aliases are supported. A conditional or exhaustive match may choose between compatible named references at runtime, including aliases bound inside its branches. Every possible target must be named and capture-free; choosing a supported branch once does not make an unsupported alternative transferable. The compiler checks complete immutable bindings after the enclosing bodies are built.

Existing exact callable signatures apply, including all ten numeric widths, zero/multiple parameters, nominal record/enum types, CPU collections, higher-order signatures and canonical expected-error sets. Imported named definitions retain their resolved identities and private helper dependencies. Shape guards and ordinary left-to-right argument evaluation remain unchanged.

A named factory is itself capture-free even when its body creates a captured function. Passing that factory to a worker and creating/invoking the closure there is supported: its environment belongs to the worker invocation. Passing a factory-produced function from the parent is outside this first provenance analysis, even when a person can see that the factory always returns a named function.

The recipient is still a statically named task entry. It may coordinate existing nested CPU work and keep its capture-free function parameter across suspension. The function value being passed must itself remain transitively synchronous. Calling that value within the worker runs an ordinary call; it does not start another task implicitly. Recursive targets retain the existing [checked call-admission rules](native-recursive-callables.md).

## Ownership and checked publication

Only executable-lifetime code and a null environment cross this boundary. There is no closure environment to allocate, copy or keep alive. The child uses its own checked invocation context, cancellation token, allocation ledger and call counter. Settling or destroying the submitting invocation after argument construction does not invalidate the code reference.

Worker callable value layouts now distinguish exact signatures. Generated copy routines compare the incoming code address against trusted compiler-emitted named checked entries of that signature and require a null environment and a completed value header. Unknown/null code addresses, wrong-signature entries, nonempty environments and failure headers are rejected before publication or indirect invocation. Validation compares pointers without dereferencing an untrusted environment. This covers both pure MLIR adapters and synchronous control-flow entries without creating a public callback registry or stable layout ID.

An already faulted or cancelled destination cannot publish a callable. Its primary fault/cancellation is preserved, and rejected copies leave output untouched. Invalid private metadata produces the existing checked internal execution fault. Ordinary scheduler admission, child failures, typed callback errors, cached outcomes and group settlement keep their existing rules.

This does not claim allocation-free tasks or callbacks. The scheduler, checked call frames, callback body and returned owned data can still allocate. Only the capture-free function handoff needs no environment allocation.

## Current restrictions

Captured closures and anonymous functions, including capture-free anonymous functions, cannot yet be sent as worker arguments. A function-valued parameter forwarded to another worker, a factory call, an aggregate projection or another value with unproven provenance is conservatively rejected. These are temporary implementation diagnostics, not new source-level function types or permanent language limits.

Functions nested in worker argument records/enums/arrays or error payloads remain rejected. Function-valued worker results, spawned-map function captures and indirect `spawn transform(...)` remain rejected. Worker-local construction and use of closures and function-containing aggregates retain their existing support. GPU callbacks, resumable callable values, distributed execution and public C/C++ integration are unchanged.

## Qualification

The [CPU qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-named-functions-2026-10-19.json) records 66 selected CTest checks qualified on Apple M2 Max with LLVM/MLIR 22.1.8. The selected run passed 64 checks; two legacy frontend tests still rejected the newly supported named arguments. Those assertions now cover unproven forwarded parameters, and both affected suites pass after rebuilding and rechecking. No compiler implementation changed during that correction. This is a focused selection, not the full project suite.

New frontend suites check/plan 16 valid programs, reject ten unsupported boundaries and reject three forged IR cases in normal and ASan/UBSan builds. New execution suites compile/run 28 Kalandra programs per mode across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds (112 source programs). Coverage includes all ten numeric widths, both sides of runtime target selection, imported helpers, higher-order factories, mixed signatures, owned records/arrays, expected errors, recursion and nested coordinating recipients.

Private harnesses check 32 callable-copy cases across those modes: valid metadata; null, unknown and wrong-signature targets; nonempty environments; failure headers; and already faulted/cancelled destinations. Invalid or stopped copies leave output untouched. The harnesses run 192 tasks and observe 384 cached results across one/two/four-worker pools, destroying the submitting invocation before scheduler drain and checking final ownership/reservation cleanup. There are 120 additional CLI worker runs and 96 allocation-failure CLI probes. Failures publish no result and report balanced cleanup; successful allocation probes return the expected value. Existing syntax, modules, maps, closures, callable aggregates, errors, recursion and local-worker regressions also pass.
