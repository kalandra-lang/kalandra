# Fair comparisons

Run from the repository root after building the native compiler and optimized
runtime. Python 3 and the pinned LLVM tools are required; source comparisons also
need Go and Rust. Numerical library comparisons use NumPy and Homebrew OpenBLAS
on the currently supported Apple Silicon benchmark host.

```sh
python3 native/benchmarks/fair/source_run.py --output benchmarks/results/source-new.json
python3 native/benchmarks/fair/libraries_run.py --output benchmarks/results/libraries-new.json
python3 native/tests/FairBenchmarkTests.py
```

Choose fresh output paths. Generated artifacts live in ignored `benchmarks/.build`;
raw measurements stay local. The [release evidence index](../../../benchmarks/README.md)
explains the retained studies, uncertainty and historical archive.

## Equivalent source algorithms

`source_run.py` compares the same recursive traversal and polynomial, branch or
Euclidean-GCD leaves in Kalandra, C++, Rust, Go and Python. Runtime parameters
prevent replacing the whole workload with a constant. `runtime_workloads.py`
adds array and closure algorithms against C++/Rust, with explicit ordinary versus
cancellable execution contracts. `wide_ordinary.py` extends the source corpus to
signed and wide runtime inputs. See each script's `--help` for build directories,
execution contracts and paired frozen/current comparisons.

These suites compare equivalent algorithms; they do not compare Kalandra's native
matrix primitive against handwritten loops and call that language parity. Source
recursion has no supported GPU implementation, so no fictitious GPU entry is shown.

## Iterator abstraction costs

`iteration.py` compares direct loops, lazy sources, filter/map adapters and
explicit growing-array materialization at 64 and 4,096 items. Build two frozen
compiler/runtime/header installations with `--build-only`, then use `--baseline`
and `--candidate` to select their build directories. Matching modes compare the
same source before/after a compiler change. Different modes measure abstraction
costs rather than interchangeable storage or suspension contracts.

`iteration_patterns.py` adds ten direct/lazy pairs: generator factories, `take`, consumer
`break`/`continue`, a `while` producer, read-only arrays, captured callbacks,
nested producers, an outlined local traversal, parameterized factories and
adapters constructed directly in a `for` header. It uses the same frozen-driver
protocol and exact per-invocation checks. Select `--baseline-mode direct` and
`--candidate-mode lazy` for abstraction costs, or `lazy` for both to measure a
compiler change. The manifest protects every executable, source, compiler,
runtime, driver and header used by a comparison. Repeat `--pattern NAME` to build
or measure only the affected patterns without repeating the entire corpus.

Run identical-build controls and independent paired/replicated comparisons
sequentially, without concurrent tests, builds, profiling or other CPU/GPU work.
`--max-iterations` bounds adaptive batches when an older producer is much slower.
Keep failed controls and unfavorable results. These numeric traversals do not
establish the performance of escaping, effectful or task-suspending cursors.

## Numerical operations and full models

`libraries_run.py` compares dot, AXPY and matrix operations against optimized
OpenBLAS APIs in C++, Rust, Go and Python, plus NumPy. Kalandra has CPU, resident
Metal and upload-inclusive paths. Full-output qualification uses independent
exact/high-precision references and detects cell-order and nonfinite failures.
GPU float32 may use optimized FMA accumulation; fixed-tree mode preserves the
previous product/reduction order. Do not require identical bits across different
numerical contracts or treat a loose error bound as proof of practical accuracy.

`../residual_ffn.py` and `../transformer_block.py` measure complete dataflows.
`../steel_integration.py` freezes and qualifies production Steel integration;
`steel_integration_series.py` runs sequential AA, frozen, policy, replication and
MPSGraph-reference cohorts, stopping if AA controls fail. The explicitly
unqualified `steel_model_diagnostic.py` retains all process outcomes after a failed
control. Reproduction details are in the [integration report](../../../benchmarks/results/steel-production-2026-10-30.md).

Run qualification first and timings alone. Validate warmups and every sample,
retain regressions and failed controls, and disclose allocations, residency,
uploads, readback, thread counts, numerical policy and cold preparation. Profiling
is a separate diagnostic pass. Never compare cancellable and ordinary entries
without identifying that difference.
