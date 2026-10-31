# Temporary CPU input and array generation experiment

Evidence-only prototype based on Kalandra 0.99.1. Original workload: https://benjdd.com/languages2/. This temporary branch is intentionally never to be merged or released. These experimental APIs are not available in published Kalandra 0.99.1.

- `args.int32(index: int) -> int32`: zero-based user argument (excluding the executable name), strict decimal parsing and checked int32 range. Missing/negative indices are BoundsFaults; malformed/out-of-range values are ArithmeticFaults, with an input diagnostic.
- `random.int32(min: int32, maxExclusive: int32) -> int32`: bounded, unbiased pseudorandom integer, seeded from `std::random_device`. Thread-safe CPU runtime; not cryptographic. Empty/reversed intervals fail with BoundsFault.
- `generate(count: int, callback: fn(int) -> T) -> [T]`: sequential indices `[0,count)`, one output allocation, immutable result. Reuses `map`'s completed-element/capture support. Negative sizes and byte-size overflow are checked. These prototypes reject Metal placement checking.

Executable arguments are initialized once before user code and remain immutable for the process lifetime. Collection executable display flags remain reserved. This is experimental API/error design, not a production compatibility commitment.

The benchmark receives runtime `u`, row count and inner count; use `40 10000 100000` for one billion logical remainder/add iterations. The array has 10,000 elements and each row uses a fresh inner accumulation. Random index selection is adjusted with modulo for small verification shapes; for the main case it is identical to the original `[0,10000)` random index.

Correctness, optimization audits and timing evidence will be recorded separately. No timing claim is valid until the optimized artifacts have been checked for repeated work and compared on the same machine.

Collection-returning executables separate program arguments from display controls:
`./app --all -- 40 10000 100000`. Scalar benchmark executables take the three
arguments directly. Existing collection display-option validation is preserved.

To verify every timing observation independently, each scalar executable prints
`r * 2^32 + selectedValue`. The runner decodes both fields and checks the selected
value against an independent arithmetic-series reference. All three languages
perform this same constant-size output packing outside the loops. `verify.k`
and reference `--verify` runs additionally expose every array element.

Run phases sequentially:

```sh
python3 native/benchmarks/billion-loops/test_apis.py
MACOSX_DEPLOYMENT_TARGET=26.0 python3 native/benchmarks/billion-loops/run.py build
python3 native/benchmarks/billion-loops/run.py verify
python3 native/benchmarks/billion-loops/run.py timing
```

Raw binaries, optimized IR/disassembly, correctness data and timing observations
are under ignored `build/billion-loops/`. Stop builds/tests before timing. The
benchmark uses runtime dimensions as well as runtime `u`; the main case is
`40 10000 100000`. Only positive `u`/row counts and nonnegative inner counts with
non-overflowing int32 results are compared. The reference programs retain the
original zero initialization; `generate` writes each output once without a
separate zero-fill. C++/Rust ordinary optimized controls and checked-overflow
controls are both retained. Random-source implementations differ; all perform
one bounded draw before the measured arithmetic work.

The timing protocol uses seven shuffled complete blocks, row-count scaling,
one warmup per executable and an identical C++ binary repeated as a noise
control. All outputs and frozen executable hashes are checked. Background
process CPU snapshots are retained; a 400% pre-run CPU stop threshold bounds
heavy competition on this 12-core Mac. macOS maintenance may remain active even
when user workloads are idle, so these are not automatically uncontended claims.
The collector waits at the pre-run load gate; `timing --resume` retains completed samples and the original order after an interruption. An identical-binary median difference above 5% makes the control inconclusive.
No measurements are compared directly with the linked website's different Mac.

Measured results and qualification details: [REPORT.md](REPORT.md).

Published measurement evidence is in [evidence/](evidence/), including all unfavorable samples and the interrupted study. Process names and PIDs are omitted for privacy; CPU percentages, timings, outputs, ordering and hashes are retained. Generated binaries and full IR/disassembly stay in ignored build output; reproduce them with the commands above. The [video renderer](video/) uses these same primary medians.
