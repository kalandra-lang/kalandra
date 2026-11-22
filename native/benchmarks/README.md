# Native benchmark runners

Build the native compiler and optimized runtime first:

```sh
cmake --preset native
cmake --build --preset native --parallel 2
```

The [fair suite](fair/README.md) is the supported starting point for comparisons
with C++, Rust, Go, Python and optimized numerical libraries. It keeps equivalent
algorithms separate from library-level operations and verifies results before
measurement. [Current results and limitations](../../benchmarks/README.md).

## Component measurements

| Runner | Measures |
| --- | --- |
| `run.py` | CPU generated matrix kernels and task lifecycle |
| `scheduler.py` | Scheduler phases and separately instrumented diagnostics |
| `shapes.py` | Rectangular CPU matrix shapes |
| `gpu.py` | Source-generated CPU/Metal numerical workloads |
| `matrix_run.py` | Matrix operations and independent numerical references |
| `pipeline_run.py` | Matrix/post-map/reduction pipelines |
| `residual_ffn.py` | Complete residual feed-forward block |
| `transformer_block.py` | Complete transformer block and MPSGraph reference |
| `steel_integration.py` | Frozen production matmul integration qualification |

Use each runner's `--help` for arguments and choose a fresh output path. Python 3,
the pinned LLVM/Metal tools and a built compiler are required; numerical references
also use NumPy and some library comparisons use OpenBLAS. MPSGraph/Accelerate are
benchmark references only, never production dependencies.

Run on an idle machine. Do not overlap timings with builds, tests, profiling or
another benchmark. Keep numerical checks and device acknowledgement, distinguish
resident inputs from uploads, record numerical policy and disclose readback and
cold setup. Raw outputs and `.build/` are ignored; preserve the original data when
writing a reviewed summary. Retired research scripts are recoverable through the
[archive record](../../benchmarks/research-archive.json).


## Native optimization levels

Run `python3 native/benchmarks/optimization-levels/run.py /path/to/kalandra timings.json`
from the repository root. It measures end-to-end compile time, executable size and
whole-process runtime at all four levels. Each shuffled block repeats O2 as a
control. The three programs check a billion integer remainder iterations, binary
recursion and floating-point accumulation against independent expected results.
[PR #332](https://github.com/kalandra-lang/kalandra/pull/332) records the M2 Max measurements.
