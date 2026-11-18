# GPU text prefix benchmark

The public benchmark applies the same first-scalar predicate to every row. It
uses short ASCII and multibyte UTF-8 strings, combining marks and empty rows.
It checks every result against the expected mask after each timed interval.
The CPU measurement starts with host strings and stops with a host mask. The
GPU measurement also includes packing, upload, task submission, completion
and download. Each measured application is a fresh process. The clock starts
after input generation, and the GPU's first launch is part of the interval.

```sh
python3 native/benchmarks/gpu-text/prefix.py \
  --compiler /path/to/kalandra --backend metal --directory /tmp/text-prefix
```

Use `--backend vulkan` or `--backend cuda` with a compiler and GPU that support
that backend. The script builds first, then runs alternating ABBA and BAAB
blocks in sequence. The JSON report retains all samples, observed desktop load,
compiler and shader hashes, and the result check for each observation. Keep
builds and profiling outside the timing window.

`Resident.cpp` measures the same compiled cooperative shader after both packed
inputs are resident. It includes GPU task submission, completion, observation
and retirement. Each mask is downloaded and checked after timing. Build it with
the matching backend's headers and optimized runtime archives, as for the
cooperative benchmark driver. Define `BENCH_METAL` for Metal, `BENCH_CUDA` for
CUDA, or neither for Vulkan. Link `native/runtime/String.cpp` for native text
construction. Pass the generated shader path, its row count and the number of
repetitions. The first reported sample is a warmup. Compare this measurement
separately from the public end-to-end numbers.

Run the same driver with `--native-cpu` in place of the shader to measure a
linear CPU prefix scan over validated input. The public CPU `String` array
currently revalidates the whole array on indexed access, so its timing also
reflects that separate cost. The native scan gives a second CPU reference.

### Initial Metal results

On an Apple M2 Max, five public ABBA/BAAB blocks and three independent
resident/reference processes gave the following medians. Every measured mask
matched the independent expected output. The two GPU columns use different
measurement boundaries.

| Rows | Kalandra CPU, public | Metal, including transfers | Metal, inputs resident | Linear native CPU |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 44.1 µs | 4.29 ms | 263 µs | 7 ns |
| 1,024 | 45.5 ms | 4.37 ms | 198 µs | 424 ns |
| 65,536 | — | — | 222 µs | 26.8 µs |

The linear CPU reference wins on this small predicate. The 65,536-row public
CPU trial was stopped due to repeated array validation. A 4,096-row public
Metal trial reached the existing scheduler memory limit. The same diagnostic
program succeeded with a larger limit in a private build. Production uses
the existing limit. Split input into batches when the task's memory budget
cannot hold the complete public input and its retained backing.
