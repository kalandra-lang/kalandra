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
