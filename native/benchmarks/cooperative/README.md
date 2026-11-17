# Cooperative dispatch qualification

These tools compare ordinary numerical dispatch against an actual released
runtime, and cooperative stencil dispatch against independently compiled native
MSL, GLSL and CUDA C++ implementations. Production code does not depend on a
vendor numerical library.

`Driver.cpp` compiles against **each snapshot's own headers and static archives**.
Do not combine released libraries with current headers: private runtime layouts
can change. Define `BENCH_METAL` or `BENCH_CUDA`; without either it uses Vulkan.
Define `BENCH_WORKGROUP` for the shared-memory stencil, otherwise use the ordinary
`(values + 1) * 2` source in `ordinary.k`.

All paths use the same task admission, resident inputs, dispatch, observation and
retirement. Transfers and shader loading are outside the measured interval. Metal
prepared-output caching is disabled so every invocation performs the work. Each
process has a fixed one-second warmup, followed by two reported warmup batches.
Every batch's output is checked against an independent CPU reference outside its
timing. Float32 outputs are bit-exact except Vulkan division, which is checked
against its existing 2.5-ULP contract. Its accepted float intervals are calculated
before GPU warmup and rechecked at both endpoints and excluded neighbors against
the original long-double predicate. Every batch still checks every output, with
exact zero signs and no acceptance of NaNs or infinities. This avoids repeated
libm calculations introducing long CPU-only gaps between GPU batches. Input
values and sizes make both stencil additions exact. Every invocation checks task
ownership retirement. `VulkanStencilCheckTests.cpp` compares the interval check
against the original predicate for all 109 possible fixture sums, both zero
signs, neighboring floats, infinities and NaNs:

```sh
clang++ -std=c++23 -O2 -ffp-contract=off \
  native/benchmarks/cooperative/VulkanStencilCheckTests.cpp -o /tmp/stencil-check
/tmp/stencil-check
```

The native stencil references have the same tile/halo, group size, padding,
barrier and arithmetic sequence on valid inputs. They omit the compiler's general
checked-arithmetic/fault machinery and are a code-generation lower bound, not a
claim that their failure contract is equivalent to Kalandra's. Compile Metal with
fast math and FP contraction disabled. Compile GLSL with `glslc --target-env=vulkan1.0
-O`, then use `reference_artifacts.py` to rename only its SPIR-V entry-point string
and run `spirv-val`. Compile CUDA with the installed device-only NVRTC using
`nvrtc_reference.py`; its options disable FMA and select precise division. This
avoids a host C++ toolchain dependency. `reference_artifacts.py` adds the checked
private CUDA metadata envelope without modifying PTX instructions.

`acceptance.py` takes a JSON configuration with `platform` (`macos` or `linux`)
and four commands, each an `[executable, shader]` pair:

```json
{
  "platform": "linux",
  "commands": {
    "ordinary-baseline": ["/absolute/frozen-driver", "/absolute/ordinary.ptx"],
    "ordinary-current": ["/absolute/current-driver", "/absolute/ordinary.ptx"],
    "cooperative-current": ["/absolute/cooperative-driver", "/absolute/generated.ptx"],
    "cooperative-reference": ["/absolute/cooperative-driver", "/absolute/reference.ptx"]
  }
}
```

Run from the repository root, with fresh output paths. An optional `prefix` command (for example
`["/usr/bin/taskset", "-c", "2,3"]` after checking the actual CPU topology)
restricts only benchmark processes to homogeneous cores on hybrid Linux hosts;
it must be identical for all variants and recorded in the configuration.
Finish all builds,
correctness tests and reference validation first. Never overlap acceptance with
builds, tests, profiling, plotting or another acceptance run.

```sh
python3 native/benchmarks/cooperative/acceptance.py \
  --configuration /absolute/configuration.json \
  --protocol native/benchmarks/fair/protocol.py \
  --study aa --output /absolute/new-aa.json
```

The studies are `aa`, `frozen`, `repeat`, `cooperative-aa`, `reference` and
`reference-repeat`, run sequentially. Both identical-build controls must pass
before interpreting their respective comparisons. The fixed control limits are
5% median paired time change and 10% worst paired-block change. The common fair
protocol uses three adjacent ABBA/BAAB blocks, five measured rounds, equal paired
iteration counts and adaptive 100-ms batches at 16, 65,537 and 1,048,576 elements.
The frozen comparison protects existing ordinary dispatch; it cannot benchmark a
cooperative API absent from the release.

The orchestrator records every calibration, warmup, measurement, command, artifact
hash and external CPU-load sample, including failed attempts. CPU samples use cumulative process-time deltas on both
hosts, with Linux counters from procfs and macOS counters from ps TIME. Intervals
are at least 500 ms, avoiding quantized sub-tick samples and lifetime-average
percentages for short-lived processes. The CPU gate rejects more than two external cores in total
or more than 90% of one core from one external process. Linux also requires three
initial NVIDIA utilization samples at or below 2%. These gates do not prove that
all shared GPU resources remain idle: inspect the controls and replication too.
Initial admission tries at most five three-sample idle windows.
A process rejected by the CPU-load gate is retained and retried at most five
times, with a two-second pause; correctness and timing-control failures are never
retried this way. Do not delete unfavorable outcomes or lower thresholds after
seeing results.

`PairedDriver.cpp` and `paired_reference.py` provide an additional native-reference
experiment when creating a fresh driver context for each variant is unstable.
The driver loads both kernels into one runtime and alternates them in three
ABBA/BAAB blocks, using the same resident input and task lifecycle. Each size has
three independent process replications. Compile with the same backend definition
and archives as the ordinary driver. Run `--study aa`
first, then `--study reference` and `--study repeat`, passing the accepted control
with `--control`. Configuration, driver, shader and monitor hashes must match.
The 5% median and 10% worst-block limits and CPU/GPU admission gates are unchanged.
Each process is limited to 180 seconds; output is recorded in files so collecting
measurements cannot block on a full pipe. This experiment does not replace or
erase failed fresh-context controls, and does not measure kernel-only time.

For measurements alongside normal desktop work, both orchestrators accept
`--load-policy record`. This profile records CPU/GPU activity without refusing,
discarding or retrying a batch because of competing work. All measurements remain
in the analysis, including contended batches. The numerical/ownership assertions,
ABBA/BAAB ordering and fixed 5% median / 10% worst-block identical-build limits are
unchanged. Reference comparisons require a passing control under the same profile
and artifact hashes. Keep these results separate from idle-machine studies and
label them as measurements under the observed workload, rather than uncontended
GPU throughput. A failed timing control means the comparison is inconclusive; it
does not justify selecting quieter results or asking users to stop their work.
