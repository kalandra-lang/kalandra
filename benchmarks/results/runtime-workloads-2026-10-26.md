# Expanded CPU parity measurements — 26 October 2026

The fresh production recursion run still meets the 10% C++/Rust target in all 30 non-empty comparisons. That does not generalize to the new array and closure workloads: repeated eight-element array construction is 30–55% slower than C++ and 34–50% slower than Rust at the measured nonzero round counts, while 4,096 closure steps are 12.1%/11.4% slower. Short closures expose a much larger fixed cost. No CPU production optimization is included in this change.

## Runtime-input source algorithms

Equivalent runtime-input algorithms: eight-element immutable array maps with a fresh output each pass, and a scalar-capturing closure repeatedly passed through a tail-recursive helper. Each language may optimize normally; C++/Rust use templates/generics, with no forced dynamic calls. Kalandra includes fresh invocation, cancellation, checked arithmetic, allocation/ownership and settlement. C++/Rust retain normal semantics. Identical external C++ timing/validation harness; separate objects/no LTO. Exact independent integer oracle checked every call. Task diagnostic is Kalandra-only, fixed-input complete parallel-scope creation/spawn/await/destruction versus a fixed-input synchronous entry; scheduled explicitly uses 8 workers, scheduled-serial uses the host-driven 1-worker scheduler. It is not a steady-state scheduler microbenchmark or a cross-language ranking.

Three shuffled fresh processes per cell, two full warmup batches and seven measured batches, 20 ms calibration target, at most 20,000 iterations. Table values are medians of process medians. Every call is checked against the independent integer oracle, with 90 edge/reference workloads across three seeds plus nine fixed-input diagnostic checks. Startup, compilation and profiler time are excluded. No affinity/frequency/desktop isolation.

| Algorithm | Rounds | Kalandra ns | C++ ns | Rust ns | K/C++ | K/Rust |
|---|---:|---:|---:|---:|---:|---:|
| eight-element array maps | 0 | 120.23 | 22.40 | 22.77 | 5.367 | 5.280 |
| eight-element array maps | 16 | 667.82 | 429.78 | 445.89 | 1.554 | 1.498 |
| eight-element array maps | 64 | 2527.14 | 1944.20 | 1885.40 | 1.300 | 1.340 |
| eight-element array maps | 256 | 10064.10 | 7602.57 | 7284.11 | 1.324 | 1.382 |
| captured closure | 0 | 117.41 | 4.46 | 1.47 | 26.298 | 79.939 |
| captured closure | 16 | 181.30 | 28.97 | 28.56 | 6.258 | 6.347 |
| captured closure | 256 | 1210.90 | 953.49 | 959.60 | 1.270 | 1.262 |
| captured closure | 4096 | 17668.18 | 15765.87 | 15866.63 | 1.121 | 1.114 |

Zero rounds are explicitly fixed-cost controls, not useful-work parity evidence. The array workload measures many tiny immutable allocations rather than memory bandwidth. C++ and Rust can specialize closure calls normally; their kernels contain no forced dynamic dispatch. Kalandra retains cancellation, overflow checks, owned environments and invocation settlement. These intrinsic differences are disclosed, not disabled.

## Complete task-scope diagnostic

Kalandra only, fixed seed 17 and compiled round count. Each call creates and drains the generated task runtime. The one-worker mode drives task execution on the host but still creates the cancellation-monitor thread on this platform; it is not thread-free. A fresh eight-worker pool has additional initialization/teardown. This is not a steady-state task queue benchmark and does not compare C++/Rust schedulers.

| Closure steps | Synchronous µs | Task scope, 1 worker µs | Task scope, 8 workers µs |
|---|---:|---:|---:|
| 0 | 0.122 | 23.306 | 222.115 |
| 16 | 0.184 | 27.512 | 280.829 |
| 256 | 1.219 | 56.467 | 1104.828 |

## What the separate profile identifies

The array sample shows the generated loop plus malloc/free, invocation allocation/release and poll helpers. The zero-step closure sample spends many samples beneath `Invocation::reserve()` in `getenv("KALANDRA_TEST_FAIL_ALLOC_AT")`, followed by environment allocation and settlement. This lookup occurs at the first allocation of each fresh invocation, not every allocation within it. Sampling observations are not exact exclusive time percentages.

The task-callable path has a more fundamental difference. The emitted `kalandra_callable_plan_3` for `applyRounds` allocates a 496-byte frame, calls scalar helper wrappers with status/poll checks, recursively calls itself, then frees the frame and leaves the checked call boundary. The native synchronous benchmark uses the separate optimized native lowering. The one-worker profile contains that callable frame path, allocation/memset/checked entry, and cancellation-monitor thread startup. Pool creation therefore explains only part of the task-scope difference; it cannot explain all growth with closure steps.

Next optimization priorities are non-escaping closure environments and tiny-array ownership/allocation, followed by lowering proven non-suspending task callables through the native call path and avoiding repeated scheduler/monitor creation when lifetimes permit. Fault injection, checked errors, cancellation and ownership must remain tested; blindly caching the environment setting would change its current per-invocation test behavior.

## Refreshed recursion results

Fresh C++/Rust references are rebuilt from the original depth-study sources. Kalandra uses production compiler entries and the unchanged optimized task runtime; the native driver now explicitly rejects nonfinite results. Seven randomized mirrored blocks, identical iteration count per cell, two full-batch warmups. Kalandra checks every invocation within two ULP and balanced cleanup; measured aggregate checksums in all implementations satisfy the independent Fraction-oracle rounding bound. Invocation lifecycle is timed; scheduler creation/dispatch is excluded. This production hybrid path is qualified here on Darwin ARM64/M2 Max, not Linux/Windows.

| Program | Depth | Hybrid/C++ | Hybrid/Rust |
|---|---:|---:|---:|
| polynomial | 10 | 0.747 | 0.703 |
| polynomial | 14 | 0.723 | 0.682 |
| polynomial | 18 | 0.739 | 0.693 |
| polynomial | 22 | 0.728 | 0.684 |
| branches | 10 | 0.798 | 0.801 |
| branches | 14 | 0.791 | 0.792 |
| branches | 18 | 0.795 | 0.792 |
| branches | 22 | 0.790 | 0.791 |
| euclid | 10 | 0.966 | 0.809 |
| euclid | 14 | 0.952 | 0.740 |
| euclid | 18 | 0.940 | 0.733 |
| euclid | 22 | 0.958 | 0.742 |
| chain | 0 | 9.577 | 4.706 |
| chain | 100 | 1.063 | 1.062 |
| chain | 1000 | 1.045 | 1.040 |
| chain | 10000 | 1.031 | 1.032 |

## Evidence and reproduction

Run `python3 native/benchmarks/fair/runtime_workloads.py --output <unused.json>` and `python3 native/experiments/cancellation/production_entry.py <unused.json>`. Run `runtime_workload_profile.py --output <unused.json>` separately; it samples only its own child processes and checks their outputs. The profile does not supply acceptance timings.

Development fixture failures are retained in the build/run logs: placing a task function in the scalar test module selected the task test ABI; a probe initially selected the wrong entry symbol; its task link initially lacked the production ABI shim. The final fixtures separate scalar/task modules, use the correct generated entry, link the production TaskABI implementation, and prevalidate all diagnostic entries before measuring. These were benchmark-harness failures, not reclassified as language correctness passes.

- [Array/closure/task raw results, sources and hashes](runtime-workloads-2026-10-26.json)
- [Recursion refresh](production-cancellation-refresh-2026-10-26.json)
- [Separate profile manifest](runtime-workload-profile-2026-10-26.json)
- [Array sample](runtime-workload-profile-2026-10-26.arrays.sample.txt)
- [Closure sample](runtime-workload-profile-2026-10-26.closure.sample.txt)
- [Task-callable sample](runtime-workload-profile-2026-10-26.scheduled-256.sample.txt)
- [Numerical/statistical audit](runtime-workloads-final-audit-2026-10-26.json)
