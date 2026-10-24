# Native CPU scaling benchmark

Apple GPU measurements have a separate [CPU/Metal baseline](../../benchmarks/results/native-gpu-2026-10-15.md) and [runner](#apple-gpu-baseline).

For equivalent source algorithms and optimized OpenBLAS/Accelerate APIs, use the [fair comparison suite](fair/README.md).

This measures the native C++/LLVM implementation. The [language comparison suite](languages/README.md) separately compares native Kalandra CPU/Metal programs with Go, Python, Rust and C++. See the [initial report](../../benchmarks/results/native-cpu-2026-10-15.md) and [scratch reuse comparison](../../benchmarks/results/native-cpu-2026-10-15-scratch.md).

Build the compiler first, then run:

```sh
cmake --preset native
cmake --build --preset native --parallel 2
python3 native/benchmarks/run.py
```

Python 3 and the pinned Clang/LLVM installation are required. Neither Bun nor Node is needed. Optional arguments select `--compiler`, `--clang`, and `--output`. The default output is `benchmarks/results/native-cpu-YYYY-MM-DD.json`; another run on the same day overwrites it. Build artifacts live in ignored `benchmarks/.build/native-cpu/`. Run with other CPU-heavy work stopped.

The runner compiles the existing dynamic projection fixture with `--test-entry reference -O2`, then compiles its emitted LLVM, the C++ harness, and runtime together with Clang `-O2 -DNDEBUG -ffp-contract=off`. Thus the normal Debug CMake runtime archive is **not** the measured runtime. Compilation/linking are timed separately. There is no LTO, fast math, sanitizer, BLAS, or GPU in the performance build.

## Measurement

Each batch performs sixteen identical, independent operations: multiply two square float32 matrices, then sum the resulting matrix. Dimensions are 8, 32, and 96. A separate dimension-zero control returns the constant 1 without entering a generated kernel; it measures the same task lifecycle with negligible computation, not empty-matrix semantics.

Inputs are initialized at runtime with nonuniform, exactly representable binary fractions. An independent double-precision triple loop calculates the expected scalar. For these inputs/sizes the generated balanced float32 reductions must equal the rounded reference exactly. Every result is checked after timing, including all warmups, and inputs are checked unchanged after each workload. Every batch also checks that scheduler-owned bytes, tensor owners, unfinished work, task records, and groups return to zero. Allocation/release counts come from an untimed diagnostic invocation; no atomic poll-observation probes are enabled.

The five configurations are direct sequential calls and TaskCore with 1, 2, 4, and 8 workers. **One worker uses the host-driven serial scheduler; it does not create a background thread.** Larger configurations create persistent background pools. Every batch uses a fresh root group on its existing pool. Pool construction and input/reference setup are outside the timed batches. Construction samples include thread creation requests, not a guarantee that every idle worker has reached its wait; warmups precede measurements. Teardown is also excluded.

Timing includes invocation setup/cleanup for direct calls. Scheduled batches include group creation, synchronous packet construction, admission, execution, result copying/observation, group close/drain, and release. Failure checks necessary to consume the results are inside this interval; numerical equality and quiescent resource checks are outside it. The scheduler uses default table/queue budgets. Kernel inputs are shared immutable host buffers that outlive every task; this does not measure tensor packet transport or retained ownership across a source-level spawn.

Each fresh process runs three warmup batches per configuration and nine measured rounds per workload. Configuration order is deterministically shuffled within each round. Three independent processes repeat this protocol. Tables report the median of the three process medians. JSON retains all 27 batch samples per cell, each process median, extrema, startup observations, allocation counters, cumulative peak-running/frame-result-byte statistics, toolchain/build commands, source/compiler/LLVM hashes, revision, and worktree status. Processes and workloads run sequentially. No CPU pinning, frequency control, timer subtraction, GC, or minimum-sample selection is used.

This benchmarks real generated numerical code behind a private C++ TaskEntry adapter. It does **not** time source-generated spawn/await continuation machinery, nested scheduling, cancellation latency, remote/GPU execution, or compare against another language. There is no JIT; warmups address pool readiness, allocator state, and caches. Desktop scheduling, mixed performance/efficiency cores, thermal behavior, and the tiny sample times limit generalization. No performance thresholds are added to CTest.

## Compare a compiler change

Save the baseline executable before rebuilding the compiler, then pass that copy to the runner:

```sh
# After running the baseline version of the suite:
cp benchmarks/.build/native-cpu/scaling benchmarks/.build/native-cpu/scaling-baseline
# Make the compiler change, then rebuild it:
cmake --build --preset native --parallel 2
python3 native/benchmarks/run.py \
  --baseline benchmarks/.build/native-cpu/scaling-baseline \
  --output benchmarks/results/native-cpu-comparison.json
```

Use matching harness/workloads/runtime build options in both binaries. The runner rejects using the output executable itself as the baseline, since compilation overwrites that file. It runs baseline/current, current/baseline, baseline/current as sequential fresh-process pairs, retaining both sets of samples and executable hashes. This alternation reduces ordering bias; it does not control CPU placement or thermal state. Compiler lowering and checked-ABI source hashes are also recorded for the current build. Keep the original baseline report to identify its source/toolchain/LLVM provenance. An existing saved executable is optional; ordinary single-version runs still work.

## Scheduler diagnostics

See the [initial scheduler profile](../../benchmarks/results/native-scheduler-2026-10-15.md) and [idle-wakeup comparison](../../benchmarks/results/native-scheduler-2026-10-15-wakeups.md).

```sh
python3 native/benchmarks/scheduler.py
```

This separate C++ runtime benchmark requires only Python and the pinned Clang; it does not compile a numerical program or need Bun/Node. It builds a normal `-O2` timing executable and a second executable with `KALANDRA_TASK_PROFILE`. All runtime translation units and the harness use the same macro setting. The probes and private `TaskProfile` API are absent from ordinary builds. CTest separately validates the profiling build under TSan.

Each batch admits sixteen tasks returning 1, either in one resume or after seven yields (eight resumes total per task). The host separately times admission, execution/drain, result observation, close/drain, and group release. Numerical assertions and zero-resource checks validate every batch. Results are checked during observation, so that check is included in the observation phase. Resource-statistics scans occur after the final timestamp. Both tasks and tables remain bounded; no quota is exhausted.

The controls use 1/2/4/8 workers and seven record/group capacities: 16/1, 32/1, 33/1, 128/1, 16/32, default 128/32, and 1024/256. Earlier reports used five, omitting 32/1 and 33/1. Queue, in-flight, depth, and byte budgets remain at their defaults. Only sixteen records and one root group are live, so the spare capacity changes scanning/cache costs without changing the submitted work. One worker still drives on the host. This is neither source-generated continuation throughput nor useful numerical work.

Three shuffled passes run each configuration in its own process, with one core/pool at a time. Each timing process warms up with ten batches, then records nine samples, each the arithmetic mean of ten batches. Reports use the median of the three process medians of total batch time. Each phase is summarized independently; phase medians need not sum exactly to the total median. Timer calls add overhead and no empty-timer subtraction is applied. Pool creation/destruction are excluded. Diagnostic processes are separate: one fresh core executes one batch and checks resume/publication, allocation/free, scan, and cleanup invariants. They include initial worker startup/parking observations. No diagnostic duration is used as a performance sample.

Probe meanings and limits:

- `recordVisits`/`groupVisits` count entries examined by primary maintenance passes. Historical reports count all reserved slots per pass; occupancy-indexed maintenance counts unfinished tasks and live groups. Calls with no waiting or settling tasks now skip the task scan; `taskPasses` and `idleTaskPasses` separate executed and skipped task scans and sum to `maintenancePasses`. Neither includes bitmap-word traversal, nested child/owner scans, cancellation propagation, admission searches, disposal scans, or statistics inspection. Compare these as examined-record counts, not complete scheduler operations. `readyVisits` counts full ready-table sweeps in historical reports; it is zero with event-based runnable tracking. Current probes also count `runnableEvents`, `readyPushes`, `pendingPromotions`, and `peakPending` (actual deferred backlog, excluding immediate ring placement). They establish a lower bound on entries examined.
- `locks` counts guarded mutation acquisitions, initial worker acquisitions, and explicit reacquisitions after callbacks. It excludes const inspection locks, the snapshot lock, stop locks, and condition-variable reacquisitions. `contendedLocks` means `try_lock` failed; summed `lockWaitNs` measures the subsequent blocking acquisitions, with probe overhead.
- `notifications` counts actual notify calls, not actual woken threads. Current diagnostics split these into `workerSignals` (`notify_one`), `workerBroadcasts` (`notify_all`), and `controllerSignals` (`notify_one`); historical reports before targeted wakeups used broadcasts only. `workerWaitReturns` and `controllerWaitReturns` split the completed waits by channel. Wait/return counts include spurious wakes and initial parking. `waitNs` sums completed condition-variable wait intervals, including mutex reacquisition and startup; outstanding waits have no completed duration yet.
- `allocationAttempts`/`frees` concern scheduler frame/result buffers. `allocatorNs` brackets their malloc/free calls, excluding invocation owners, core/table/vector/thread allocations, user allocators, context initialization, and cleanup callbacks.

Clock probes and `try_lock` change scheduling in the diagnostic build. Per-thread wait intervals overlap and must not be added together, divided by uninstrumented wall time, or presented as a percentage breakdown. Use event counts, uninstrumented phase timings, and capacity controls together to select the next experiment. Detailed OS profiling is still needed to attribute thread wakeups versus context switches. Raw observations, build commands, source hashes, machine metadata, and summaries are retained in `benchmarks/results/native-scheduler-YYYY-MM-DD.json` (overwritten on another run that day).

For a scheduler before/after comparison, save `benchmarks/.build/scheduler/timing` before changing the runtime, then pass the saved copy and a distinct report path:

```sh
cp benchmarks/.build/scheduler/timing benchmarks/.build/scheduler/timing-baseline
# Change/rebuild the runtime, then:
python3 native/benchmarks/scheduler.py \
  --baseline benchmarks/.build/scheduler/timing-baseline \
  --output benchmarks/results/native-scheduler-comparison.json
```

The runner rebuilds both current timing/profiling executables, then alternates saved/current and current/saved process order across the three passes for each configuration. Baseline and current phase samples, summaries, and executable hashes are retained together. A saved executable must use the same timing harness/workloads. `--output` defaults to the dated scheduler report; use a distinct path to preserve an earlier baseline. The updated diagnostic harness also asserts that idle admission, observation, close/release, and an initially empty drain emit no broadcasts. Its extra initial empty-drain proof is outside performance timing; it contributes one maintenance pass to current diagnostic scan counts.

The targeted-wakeup regression additionally checks one worker broadcast per nonempty drain activation (none for serial driving), no single-worker notification calls, and a controller wake/return on the threaded diagnostic drain. The worker and controller notification totals must sum to the aggregate. Timing binaries compile out these assertions and all probe counters. See the [targeted-wakeup comparison](../../benchmarks/results/native-scheduler-2026-10-15-targeted.md) for the next native-only before/after experiment.

The [bounded runnable tracking comparison](../../benchmarks/results/native-scheduler-2026-10-15-runnable.md) removes ready sweeps while retaining primary maintenance scans. Current diagnostic flat batches assert zero ready visits and exactly one enqueue/ring push per task resume. Runtime tests separately force a one-slot ring with a deferred backlog. Quiescent checks now require ready, deferred, and waiting/settling maintenance counts to be zero. These added checks and probes do not enter the measured timing interval.

The [occupancy-indexed maintenance comparison](../../benchmarks/results/native-scheduler-2026-10-15-maintenance.md) skips empty slots and settled task records in primary maintenance, and indexes live entries for nested checks, failure propagation, and reclamation. Bitmap storage is fixed at core creation; word traversal and remaining live-entry passes are still work.

The [task-maintenance fast-path comparison](../../benchmarks/results/native-scheduler-2026-10-15-dense.md) adds 32/1 and 33/1 capacity controls to the existing grid. Pass `--reference` alongside `--baseline` to compare an older saved executable too; the runner rotates reference/baseline/current order over three trials so each runs in every process position. It retains reference samples, phase summaries, and the reference executable hash separately. The additional modes use the same sixteen-task workload and warmup/sampling protocol.

The [four-column SIMD comparison](../../benchmarks/results/native-cpu-2026-10-15-simd.md) uses the same saved/current numerical workload and runtime to isolate code generation. It records arithmetic/assembly evidence, scratch-size tradeoffs, per-cell tail conformance, and paired before/after samples. No scheduler timing change or general BLAS/GEMM performance claim is implied.

## Rectangular, tiny, and tail-heavy shapes

The [shape report](../../benchmarks/results/native-shapes-2026-10-15.md) adds 37 synthetic matrix shapes across direct and 1/2/4/8-worker execution. It includes singleton contractions, narrow rows, tall/narrow outputs, wide outputs, deep odd contractions, all scalar-tail widths, old square controls, true empty matrices, and a separate no-kernel control. This is workload-shaped kernel evidence, not an application trace or a comparison with BLAS/GPU libraries.

```sh
python3 native/benchmarks/shapes.py \
  --baseline-llvm benchmarks/.build/native-cpu/kernels-before-simd.ll \
  --sanitize-check \
  --output benchmarks/results/native-shapes-2026-10-15.json
```

The saved scalar LLVM comes from `0fac4c0` (generate the dynamic projection fixture with `--test-entry reference --emit-llvm path -O2` using that compiler). Recreate it from that source revision if the ignored artifact is unavailable. The runner copies the supplied IR into its own ignored `benchmarks/.build/native-shapes/` directory and rebuilds **both** versions with the same [shape harness](ShapeScaling.cpp), runtime, and flags. The old square-only benchmark is unchanged.

Both variants first run every shape/mode with `--verify`; results compare by float32 bits against independently materialized, separately rounded products and newly constructed adjacent-pair levels. Expected values are not a double-precision grand total. Empty matrices actually call the generated kernel and return positive zero; only the explicit control bypasses it and returns 1. Arrays remain unchanged, cached results are checked, and all task/owner/frame/queue/maintenance counts settle to zero. Diagnostics record managed allocations/releases per case.

`--sanitize-check` additionally builds current LLVM with compiler-generated ASan attributes and C++ ASan/UBSan, checks post-Clang memory instrumentation, and verifies all 37 shapes in all five execution modes before performance begins. The older scalar LLVM is verified numerically but is not retroactively sanitizer-instrumented. Sanitized verification samples are retained separately and excluded from timing summaries.

Three fresh baseline/current process pairs alternate order. Each pair uses an identical seed to shuffle shape order and measured execution-mode order. Each shape/mode warms three batches and records nine single-batch samples, each containing sixteen calls/tasks. Summaries use the median of three process medians. Pool creation, reference/input construction, verification, warmup, allocation diagnostics, and quiescent statistics checks are excluded. Reported time includes normal computation and the selected task lifecycle. Caller/platform scheduling and the extra idle pools remain part of the environment; no affinity or frequency control is applied.

The [untimed poll probe](ShapePolls.cpp) accepts `rows inner columns`. Compile it against each saved LLVM file and `runtime/Runtime.cpp` with the same pinned Clang and `-O2 -ffp-contract=off`; the [probe artifact](../../benchmarks/results/native-shapes-2026-10-15-polls.json) retains every build/query command and hash. Its atomic poll counts must not be inserted into throughput runs or used as percentages of wall time.

## Narrow dispatch and paired tails

The [two-lane follow-up](../../benchmarks/results/native-shapes-2026-10-15-pairs.md) uses the same 37-shape protocol, with the four-lane-only LLVM from `cee47c6` as baseline. Pass that saved LLVM to `shapes.py --baseline-llvm`, keep `--sanitize-check`, and choose a distinct `--output` path. Both sides are rebuilt with identical harness/runtime/flags. The report retains initial and confirmation experiments for both per-row and final once-per-matrix dispatch, with a source reconstruction patch for the intermediate implementation. The final compiler preserves per-lane arithmetic while selecting scalar/pair/full-tile row kernels once per matrix. See the report for scratch byte tradeoffs, the remaining seven/eight-column gap, and noisy or inconsistent configurations.

## Cache-layout research

The [cache-blocking evaluation](../../benchmarks/results/native-cache-2026-10-15.md) compares the unchanged generated compiler kernel with ten strict C++ layout prototypes and single-threaded Accelerate BLAS. This is an isolated research harness, not a new compiler backend or a production BLAS dependency. It requires macOS 15+ with the SDK's modern Accelerate threading API, Python 3, and the pinned Clang. The main native build still does not require Accelerate.

```sh
python3 native/benchmarks/cache_blocking.py \
  --output benchmarks/results/native-cache-reproduction.json
```

The runner generates current LLVM, builds all variants in one timing executable, verifies numerical behavior separately, repeats verification under generated-code ASan and C++ ASan/UBSan, checks prototype assembly for strict SIMD arithmetic, then launches three fresh timing processes. It records source/compiler/LLVM/binary hashes, commands, CPU/cache metadata, raw samples, per-process medians, and computed scratch payloads. `--compiler` selects a different existing compiler executable. Build artifacts remain in ignored `benchmarks/.build/cache-blocking/`; choose a distinct `--output` to preserve earlier runs. Stop other CPU-heavy work first.

Thirteen shape controls include 32/96/256/512 squares, larger tall/wide/deep rectangles, 1,025/4,097/8,193-element odd contractions, and row/column tails. Twelve variants compare generated code; unblocked C++ 1×4, 1×16, and 4×16 tiles; K blocks of 64/256/1,024 for 1×4 and 4×16; one 1×16/K256 candidate; and BLAS. All C++ candidates use four-lane vectors, clipped output lanes, cooperative loop polls, separately rounded products, and balanced adjacent reductions. Block sizes are powers of two so each aligned K block is a subtree of the specified reduction. The final block's odd leaves carry unchanged; its root is reduced with the other roots using the same tree.

Verification covers 6,468 shape/content cases per strict variant, with per-cell comparisons for C++ candidates and aggregate comparisons for generated code. It includes every tested K-block boundary, IEEE values, all partial row counts of the four-row tile, column-panel boundaries, empty outputs/contractions, unchanged inputs, and pre-cancelled research-kernel calls. NaNs compare by classification; all other strict results compare by float32 bits. BLAS is checked against an independent double oracle with tolerance on finite verification inputs; it is not the strict arithmetic oracle. BLAS threading is set and queried on the calling thread. The vendor library itself is not sanitizer-instrumented by this build.

Each sample times **one direct matmul plus final reduction**, including result/scratch allocation, release, invocation settlement, and lightweight result/status checks. Three warmups precede seven interleaved measured rounds per shape/variant, using seeded shuffles of shapes and variant order. Summary time is the median of three process medians. Reference construction, per-cell checking, input copies, and input-immutability checks are outside the timed interval. Reused immutable inputs model warm repeated work; no worker pool, CPU affinity, frequency control, or hardware cache-miss measurement is involved. Do not compare these per-operation times directly to the older sixteen-operation batch reports.

`product_scratch_bytes` is a computed payload, not observed RSS or a hardware cache measurement. Native scratch follows its current width policy; C++ scratch includes full tile capacity plus block roots; `-1` means opaque BLAS workspace. `output_and_sum_bytes` separately counts the two result/reduction buffers and does not establish their live-overlap in generated code. C++ prototypes use ordinary RAII storage and omit checked-descriptor/size-guard and TaskError lowering. Compare layouts within C++, and use native/BLAS times for context; cross-implementation ratios do not predict the speedup of a compiler port.

## Matched MLIR blocking comparison

The [MLIR port report](../../benchmarks/results/native-mlir-blocking-2026-10-15.md) compares actual generated code before and after the 4×16/K256 implementation. Save/recreate the dynamic projection LLVM from `cc8d086` (also unchanged in `30f1e2f`), then build the current compiler and run:

```sh
python3 native/benchmarks/shapes.py \
  --baseline-llvm /path/to/pre-blocking.ll --sanitize-check \
  --output benchmarks/results/mlir-blocking-shapes-reproduction.json
python3 native/benchmarks/shapes.py --suite blocking \
  --baseline-llvm /path/to/pre-blocking.ll --sanitize-check \
  --output benchmarks/results/mlir-blocking-large-reproduction.json
```

The default suite retains the original 37 shapes, sixteen-job batches, three warmups and nine samples. The blocking suite uses fourteen larger shapes, **four-job batches**, two warmups and five samples; direct and 1/2/4/8-worker capacities produce seventy configurations. A four-job batch cannot occupy all eight workers simultaneously. It includes the thirteen C++ research shapes plus `8×1025 @ 1025×8193`, whose right-hand input alone exceeds the reported 16 MiB performance-core L2. These are still repeated warm inputs, not a controlled cold-cache experiment.

Both LLVM versions are rebuilt against identical current harness/runtime/flags. Each experiment alternates process order across three fresh pairs, using matched seeds and retaining all raw observations, source/LLVM/binary hashes, allocation diagnostics, and separate ASan/UBSan verification. Summary time is the median of three process medians. Compare versions within a suite; do not compare four-job batch times directly with sixteen-job batches or the C++ study's single-operation samples. Allocation forwarding probes belong only to CTest, not timing builds.

## Apple GPU baseline

Build the native compiler with both Metal options enabled, then run:

```sh
cmake --preset native -DKALANDRA_BUILD_METAL_RUNTIME=ON -DKALANDRA_BUILD_METAL_PROOF=ON
cmake --build --preset native
python3 native/benchmarks/gpu.py
```

This requires the existing optional shader toolchain and an actual Apple GPU. The runner builds its own optimized, uninstrumented CPU/Metal timing executable; a Debug CMake runtime archive is not measured. A quick build/validation run is available with `--smoke` and writes an ignored smoke report. The full run defaults to three processes, two warmups and seven samples per workload/mode. Use `--processes`, `--warmups`, `--samples` and `--output` to select another protocol/path. Existing full reports are not overwritten. Performance runs reject enabled Metal validation or injected libraries.

Nine workloads cover square/odd rectangular matmul and rank-one reductions. Four modes separate CPU checked-call wall time, a preallocated GPU command interval and its host wall time, managed tasks with resident device inputs, and managed tasks including uploads and downloads from existing host values. All outputs and input immutability are checked, with resource retirement assertions. Allocation, readback and result-lifetime boundaries differ between controls; see the [report](../../benchmarks/results/native-gpu-2026-10-15.md#what-is-timed) before interpreting ratios. There is no JIT; compilation, pipeline creation, input/reference construction and excluded dispatch warmups precede measured samples. No production compiler/runtime profiling hooks or performance test thresholds are introduced.

The baseline executable and its two Metal libraries must be preserved together for later comparisons: the executable loads kernels from the directory passed on its command line. Before a kernel change, copy both `scaling` and `kernels/kernel*.metallib` from the ignored build directory into a separate baseline directory. Invoke the saved executable with `scaling kernels-directory warmup-count sample-count process-index`; the benchmark uses the same actual source-generated CPU functions and GPU artifacts whose hashes are retained in JSON. New experiments should use a distinct output report and match harness, data, runtime, flags and timing boundaries. Kernel changes must retain their own correctness and sanitizer gates; a successful timing run on exact finite inputs does not replace IEEE conformance.

See the [strict normalization comparison](../../benchmarks/results/native-gpu-2026-10-15-normalization.md) for the first matched compiler optimization.

### Compare a GPU compiler change

```sh
python3 native/benchmarks/gpu.py \
  --baseline-dir benchmarks/.build/native-gpu/baseline-2026-10-15 \
  --output benchmarks/results/native-gpu-comparison.json
```

`--baseline-dir` requires the saved `scaling` executable and both `kernels/kernel0.metallib` and `kernel1.metallib`. Use matching harness, inputs, runtime, build flags, ABI and dispatch geometry. The runner alternates fresh-process order baseline/current, current/baseline, baseline/current, with matching shuffle seeds. It retains both sets of raw samples, per-process medians, process order and baseline artifact hashes, and checks that both use the same device and that saved artifacts remain unchanged. This reduces ordering bias without controlling clocks, thermals or desktop load. The original baseline report identifies its source/toolchain provenance.

## Strict GPU matmul layout study

The [layout comparison](../../benchmarks/results/native-gpu-2026-10-15-tiling.md) implements an 8×8 cooperative shared-memory candidate and 2×2/2×1 register candidates as isolated MLIR research kernels. Register tiles improved some larger dense shapes but regressed small/narrow shapes; that initial study retained scalar production selection. The subsequent crossover and managed integration below qualify the bounded M2 Max profile.

```sh
python3 native/benchmarks/gpu_tiling.py \
  --output benchmarks/results/gpu-tiling-reproduction.json
```

The runner requires the existing optional Apple GPU toolchain. It qualifies all four layouts against APFloat in optimized and ASan/UBSan host builds, with Metal API validation, before running three interleaved timing processes. The eight workloads use two excluded warmups and seven samples per layout/process. Commands time preallocated resident GPU work only; transfers, runtime task lifecycle and CPU performance are not measured. Raw samples, qualification counts, toolchain/build commands and source/artifact hashes are retained. Existing reports are preserved. See the report for exact boundaries and why these eight shapes are insufficient to choose production dispatch thresholds.

## Matmul crossover and managed selection

The broader layout sweep uses 49 shapes, including odd output/contraction dimensions, long and short contractions and extreme aspect ratios:

```sh
python3 native/benchmarks/gpu_tiling.py --suite crossover \
  --output benchmarks/results/gpu-crossover-reproduction.json
```

It retains the original optimized/sanitized APFloat and guard qualification, then runs three interleaved timing processes with two excluded warmups and seven samples per shape/layout. The singleton-contraction reference preserves the product's signed zero instead of adding an initial positive zero.

The managed selection suite separately compares CPU calls, device commands, resident tasks and transfers. To rebuild the former scalar runtime with the same current harness, CPU LLVM, task core and shader library:

```sh
mkdir -p benchmarks/.build/native-gpu/scalar-source
git show d6f40c6:native/metal/Metal.mm > benchmarks/.build/native-gpu/scalar-source/Metal.mm
python3 native/benchmarks/gpu.py --suite selection \
  --baseline-metal-source benchmarks/.build/native-gpu/scalar-source/Metal.mm \
  --output benchmarks/results/gpu-selection-reproduction.json
```

The baseline direct control uses the scalar entry; its runtime source also selects scalar matmul. The current direct control uses the same profile as managed execution. Both binaries load identical linked libraries and use identical generated CPU code. Library/pipeline initialization remains outside timing. Baseline/current process order alternates. This is a controlled kernel/runtime selection comparison, not a full historical checkout rebuild. The source hash of the earlier Metal implementation and all compiler/harness/artifact hashes are recorded.

The nine selection workloads include small, singleton-row, short-contraction and outside-profile scalar controls, regular/odd two-row and quad cases, and a reduction control. Both sides explicitly use a 512 MiB managed payload budget to accommodate the largest input's host, staging and device copies; the ordinary initial suite retains the 64 MiB default. Production runtime defaults are unchanged. Every warmup/sample validates results, inputs and resource retirement. Raw output keeps all phases and sample ranges; device-command intervals are not application speedups.

The [GPU reduction scratch reuse comparison](../../benchmarks/results/native-gpu-2026-10-15-reduction-reuse.md) measures the managed runtime against the preserved preceding executable. The direct Metal reduction harness keeps its original allocation scheme as an unchanged control; it does not exercise the managed scratch reuse.

The [reduction encoder batching comparison](../../benchmarks/results/native-gpu-2026-10-16-reduction-batching.md) rebuilds the preceding runtime with the same adaptive matmul control on both sides:

```sh
mkdir -p benchmarks/.build/native-gpu/pre-batching
git show 0f994fc:native/metal/Metal.mm > benchmarks/.build/native-gpu/pre-batching/Metal.mm
python3 native/benchmarks/gpu.py \
  --baseline-metal-source benchmarks/.build/native-gpu/pre-batching/Metal.mm \
  --baseline-adaptive-matmul --warmups 5 --samples 21 \
  --output benchmarks/results/gpu-reduction-batching-reproduction.json
```

Use `--baseline-adaptive-matmul` only for earlier runtimes that already implement the current adaptive profile. Omitting it preserves the older scalar-baseline comparison behavior. The direct reduction control still uses separate encoders and fresh output buffers, so it does not measure managed batching. Historical concurrent-candidate source is reconstructed by applying [concurrent.patch](reduction-batching/concurrent.patch) to `0f994fc`; the report records its distinct protocol and hashes.

The [packed reduction parameter comparison](../../benchmarks/results/native-gpu-2026-10-16-reduction-parameters.md) compares the serial runtime before and after Apple-family count-buffer packing:

```sh
mkdir -p benchmarks/.build/native-gpu/pre-parameters
git show 0240ec8:native/metal/Metal.mm > benchmarks/.build/native-gpu/pre-parameters/Metal.mm
python3 native/benchmarks/gpu.py \
  --baseline-metal-source benchmarks/.build/native-gpu/pre-parameters/Metal.mm \
  --baseline-adaptive-matmul --warmups 5 --samples 21 \
  --output benchmarks/results/gpu-reduction-parameters-reproduction.json
```

The direct reduction control is unchanged and does not use managed parameter packing. Buffer-object counts are measured separately from requested payload bytes and warm call latency.

## Checked-call and matrix follow-up

The [fair optimization report](../../benchmarks/results/optimization-2026-10-24.md) records source-level calls, CPU packed matrix kernels and hardware Metal contraction blocks. Use [optimization_controls.py](optimization_controls.py) after the fair suites for a matched cancellation-token control and separate device/encoding counters. `native-fast-calls` and `native-cpu-benchmark-cells` qualify call safety and every output cell of the timed matrix inputs; the existing CPU/Metal optimization tests retain full arithmetic conformance.

The [scalar tail-call follow-up](../../benchmarks/results/tail-calls-2026-10-24.md) qualifies logical depth, cancellation and stack-budgeted loop specialization. Run `optimization_controls.py --cpu-only` after the fair source suite to measure token-bound calls without reusing GPU artifacts. `native-tail-calls`, `native-tail-calls-software` and `native-total-hardware-casts` retain independent semantic/numerical assertions; the tail driver optionally accepts a frozen `--baseline` compiler for exact differential qualification.

The [bounded inlining follow-up](../../benchmarks/results/checked-inlining-2026-10-24.md) measures one checked recursive expansion level, preserving the same source algorithms. To reproduce that historical control, build `bd41ea8e` as the current compiler and provide a compiler from the pre-inlining `23c30fcb` source:

```sh
python3 native/benchmarks/checked_inlining.py \
  --baseline-compiler /path/to/pre-inlining/kalandra \
  --output benchmarks/results/checked-inlining-reproduction.json
```

Both compilers link the current optimized runtime archive. Each compiler/runtime/source/binary hash is recorded; verify that runtime and baseline source match the intended comparison. `native-checked-inlining`, the hardware/software `native-checked-inlining-source-*` tests and `native-static-numerical-shapes` qualify bounded growth, logical state, floating recursion and static tensor paths. The floating source driver also accepts an optional `--baseline` for differential qualification.

The [recursive context follow-up](../../benchmarks/results/recursive-context-2026-10-24.md) compares the current compiler against `bd41ea8e`, retaining all source algorithms and arithmetic. Run measurements after tests/builds have stopped, using fresh output paths:

```sh
python3 native/benchmarks/fair/source_run.py \
  --output benchmarks/results/recursive-source-reproduction.json
python3 native/benchmarks/checked_inlining.py \
  --baseline-compiler /path/to/bd41ea8e/kalandra --observed \
  --output benchmarks/results/recursive-context-reproduction.json
```

`--observed` adds diagnostic poll-counter cells to the clear/bound-token control. This is a compiler comparison using one current runtime archive on both sides. Differential drivers now freeze and record each compiler checksum. Floating controls support `--numeric float32` or `float64`, with rounding/FMA witnesses. The fast-call test additionally exercises deep recursion with about 1 KiB left above the native stack reserve.


Use `checked_inlining.py --paired --observed` with a frozen `--baseline-compiler` for adjacent ABBA/BAAB controls when longer runs show scheduling drift. Nine blocks per algorithm/mode retain all fresh-process samples and per-block ratios, with identical iteration counts within each pair. Two warmups and three measured batches run per process. Review the spread and observed controls; competing compilation can still invalidate a precise speed claim. See the [tail-return report](../../benchmarks/results/tail-return-2026-10-24.md).


The [private recursive state report](../../benchmarks/results/recursive-register-2026-10-24.md) combines a matched unbound control with six depth phases. `source_run.py --depths 9 10 11 12 13 14 --output PATH` retains the original source algorithms and input guard (maximum depth 14), while recording the phase cliff. `native-recursive-scalars` qualifies numeric/Bool recursive results against independent references; `RecursiveScalarTests.py` additionally accepts `--baseline` for exact state comparison.
