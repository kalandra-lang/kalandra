# Function-containing CPU worker maps

A spawned `map` accepts proven synchronous function bindings, closures, arrays of functions, and records or enums containing functions. The callback's captures, input elements, and function-containing results use the same checked ownership transfer as direct CPU worker calls.

```kalandra
fn main()->int ! TaskError:
  offsets=[40]
  add=fn(value:int):offsets[0]+value
  try parallel:
    values=try await spawn map([2],add)
    values[0]
```

This schedules **one map loop in one worker**. It does not spawn a task per element. Explicit task composition remains responsible for dividing work among workers.

## Inputs, captures, and results

Anonymous callbacks may capture proven functions or function-containing aggregates. Maps may consume function arrays and return owned closures, including closures nested in record arrays. Returned environments survive the producing group and can be passed to another worker.

```kalandra
fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value

fn main()->int ! TaskError:
  callbacks=try parallel:
    try await spawn map([40],make)
  try parallel:
    values=try await spawn map(callbacks,fn(callback):callback(2))
    values[0]
```

All possible incoming function origins must have supported CPU captures. Forwarded parameters use the existing conservative whole-module provenance analysis. Unknown or unsafe origins fail compilation, including when the map input is empty. Forged typed IR cannot bypass the same transport checks.

The worker receives independently owned function environments and function-containing aggregate storage. Immutable ordinary data buffers retain their existing sharing rules. Cached task outcomes and repeated awaits retain their own observable results. Existing environment budgets, call admission, cancellation, fault reporting, and allocation-failure cleanup apply without a new runtime ABI.

Synchronous callbacks may handle expected errors internally. Unhandled fallible callbacks retain the existing map restrictions. Functions that coordinate tasks or devices are not supported as first-class callback values.

## Remaining boundaries

[Function-containing spawned slices, concatenations and length queries](native-worker-callable-collections.md) are now supported. [Function-containing worker error payloads](native-worker-error-functions.md) are now supported with proven origins. Use the separate [`parallelMap` operation](native-parallel-map.md) for parallel element execution; `spawn map(...)` retains its sequential-loop behavior. Placement-aware/GPU callbacks and resumable function values remain separate work. Distributed execution and public C/C++ integration are separate roadmaps.

## Qualification

The focused suite covers callback aliases, captured function environments, function arrays, function-containing results, repeated awaits, onward submission, nested workers, empty inputs, handled expected errors, checked faults, and all ten supported numeric widths. It also exercises worker counts 1, 2, and 4, allocation failures, rejected source output preservation, and a forged callback origin. **77/77 selected CTest checks passed.** The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-worker-callable-maps-2026-10-20.json) lists the exact checks; this is not the full project suite.
