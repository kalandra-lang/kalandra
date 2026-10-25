# Fair comparisons

Run from the repository root on an Apple Silicon Mac with the pinned LLVM/native build, Go, Rust, NumPy and Homebrew OpenBLAS installed:

```sh
python3 native/benchmarks/fair/source_run.py --output benchmarks/results/fair-source-DATE.json
python3 native/benchmarks/fair/libraries_run.py --output benchmarks/results/fair-libraries-DATE.json
python3 native/tests/FairBenchmarkTests.py
```

Choose unused output paths. Generated files are confined to ignored `benchmarks/.build/fair`. Source/build hashes and actual commands accompany raw samples. Timings run sequentially; avoid other heavy work. Three fresh processes, two warmups and seven measured batches per process use shuffled case order and a 50 ms calibration target. The median of process medians is reported. No CPU affinity, frequency lock or host isolation is claimed.

The source suite uses the same recursive binary traversal and polynomial, branching or recursive Euclidean GCD leaves in all five languages. It uses runtime parameters, observable checksums and normal compiler optimizations. Kalandra includes checked integer arithmetic and invocation/depth/cancellation guards; actual inputs avoid overflow in every language. Numerical output is exactly representable binary64. Independent closed-form/integer/Fraction references verify edge inputs and every measured/warmup batch. These are narrow recursion/control-flow workloads. General recursive GPU execution is unsupported, so it has no replacement tensor score.

The numerical suite compares complete dot, AXPY-plus-sum and matrix-product-plus-sum tasks. C++, Go, Rust, Python ctypes and NumPy share the same installed OpenBLAS; they measure API and allocation costs around a common kernel, not five independent language implementations. NumPy's OpenBLAS control symbol address is checked against the loaded library. C++ also uses modern Apple Accelerate (`ACCELERATE_NEW_LAPACK`). OpenBLAS thread counts 1 and 12 are set and queried; Accelerate SINGLE and AUTO modes are set and queried. AUTO does not promise a worker count. Kalandra CPU uses its optimized runtime archive and one thread.

Fresh output allocation, dispatch, computation and destruction/normal GC are timed. C++/Rust/Python permit uninitialized output, Go uses zero-initialized `make`. Input initialization, process startup, library loading and preinitialized reduction ones are excluded. BLAS wrappers implement AXPY with copy+AXPY then dot-with-ones; NumPy uses multiply+add then sum. Kalandra may fuse these built-ins. Those are disclosed API implementation differences, and are kept separate from source-level compiler comparisons.

For each numerical task Kalandra has CPU, resident Metal inputs and upload-inclusive Metal inputs. GPU completion/readback of the scalar is included. Float32 Metal arithmetic is hardware; float64 Metal is **explicit software fallback**, because Apple GPU hardware lacks binary64 arithmetic. Residency boundaries are not interchangeable.

Timed dyadic input checksums must match exact independent integer references without tolerance. Separate small/odd-size and non-dyadic qualification uses 100-digit Decimal products/sums, per-cell checks for library outputs, and forward-error bounds using unit roundoff and gamma(n). Kalandra's adapters expose scalar results; its existing CPU/Metal APFloat conformance tests provide separate per-cell evidence. Library FMA and reduction order can differ from Kalandra's semantics; language conformance tests are not weakened. Every warmup and measured batch is validated, and inputs are checked for mutation after timing.

`--verify-only` on the library runner builds and qualifies without timing. `--reuse-build --reuse-verification` accepts only matching source/dependency/artifact hashes and reuses that qualification. Full build commands remain in the final evidence. Do not change benchmark files between qualification and measurement.

For paired matrix-only tuning, save a baseline compiler plus the fair suite's float32 GPU executable and complete GPU bundle **before** rebuilding. Then run `python3 native/benchmarks/matrix_run.py --baseline-compiler <saved-compiler> --baseline-gpu <saved-gpu> --baseline-gpu-bundle <saved-bundle> --output <unused.json>`. Both CPU versions use the same current runtime archive; preserve compatible baseline artifacts. `--build-only` prepares and fingerprints artifacts without timing; `--reuse-build` rejects changed inputs/artifacts before measurement. Run conformance separately before acceptance timing and keep the host free of builds/tests. The [recorded matrix report](../../../benchmarks/results/matrix-tuning-2026-10-25.md) discloses the baseline revision, experiments and remaining gaps.
