# Function-containing CPU worker error payloads

Application errors crossing a local CPU worker boundary may now contain proven synchronous functions, closures, or records/enums/arrays containing them. Error declarations use ordinary field types; no worker-specific annotations are needed.

```kalandra
error E:
  Retry:
    callback:fn(int)->int

fn failer(offset:int)->int ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:int):numbers[0]+value}

fn run()->int ! (E | TaskError):
  try parallel:
    try await spawn failer(40)

fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callback}:callback(2)
      _:fail error
```

This prints `42`. The callback keeps its captured array alive after the producing worker and group have finished. A caught error or its callback can be sent onward to another worker. Repeated awaits preserve the cached expected error and independently owned observed environments.

Application errors remain distinct from task execution failures. A failure while validating or copying a payload does not publish a partial application error. Existing cancellation, checked faults, allocation failure and cleanup behavior apply. The runtime and private callable ABI are unchanged: the compiler now admits checked error-payload paths through the existing active-variant copy machinery.

## Proving possible payloads

Checking only a function's final expression is insufficient: a guard can fail earlier, or a helper can propagate an error. The bounded provenance graph now joins possible function-containing error outcomes from each called function, follows direct and resolved indirect calls, and carries origins through `fail`, catch parameters and error-family narrowing. The capture walk checks the actual payload producers and their transitive function environments.

A function declaration may include a function-containing error family without producing such an error. Actual payload origins still need proof wherever they can be produced. Recursive calls alone do not establish a concrete origin. Unsupported captures, unknown incoming function values and malformed private IR remain rejected with a capability diagnostic; rejected source preserves existing executable, LLVM and MLIR outputs.

The analysis is conservative: it joins all function-containing error producers in a function, including locally handled channels, rather than specializing branches or proving that a handler prevents a particular payload from escaping. Existing context-insensitive caller joins and aggregate-wide origin joins also remain. This can reject safe programs until provenance analysis becomes more precise; it does not add a source-level ownership rule or a permanent restriction on error payloads.

## Scope and qualification

The focused suite covers direct and indirect workers, early guards, propagation, rethrows, narrowed families, nested workers, captured closures, function/record payloads, arrays of callbacks, cached observations, onward submission, unused declared error variants, and independent function-valued success/error outcomes. It also exercises unsafe/unknown payload rejection, forged early-error origins, worker counts 1/2/4 and allocation-failure cleanup.

General task/device-coordinating function values and placement-aware/GPU callbacks remain separate work. The separate [CPU parallel-map implementation](native-parallel-map.md) now uses these payload checks for callback errors. Distributed execution and public C/C++ integration retain their separate roadmaps.

**52/52 selected CTest checks passed.** The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-error-functions-2026-10-20.json) lists the disjoint selections and coverage; this is not the full project suite.
