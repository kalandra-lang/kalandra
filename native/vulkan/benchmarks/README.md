# Prepared numerical-library benchmark

This compares Kalandra CPU matrix multiplication, prepared managed Vulkan matrix
programs, and an already installed OpenBLAS library. It is a numerical-library
API comparison, not a source-level loop or language-speed ranking. The harness
never downloads or installs a library, driver or SDK.

```sh
python3 native/vulkan/benchmarks/prepare.py --build build/native \
  --output build/vulkan-library-benchmark
python3 native/vulkan/benchmarks/measure.py --build build/vulkan-library-benchmark \
  --output build/vulkan-library-smoke --smoke
python3 native/vulkan/benchmarks/measure.py --build build/vulkan-library-benchmark \
  --output build/vulkan-library-acceptance --acceptance
```

The Linux host must already have a compatible LP64 OpenBLAS library. Override
`prepare.py --blas` and `--cxx` when their defaults differ. Physical-device ICD
selection is explicit (`measure.py --gpu nvidia|intel`); the script never uses a
software Vulkan device as GPU performance evidence. The current small fixture
uses the installed NVIDIA management utility to record background GPU activity,
including when Intel is the selected compute device.

Each engine computes the same square float32 matrix product for two alternating
immutable left operands. Dyadic and ordinary non-dyadic input patterns are
reported separately. Kalandra's separately compiled CPU object uses `-O2`,
without LTO; every shared C++ adapter uses `-O3`. OpenBLAS uses its existing
optimized implementation with verified one-thread and 16-thread settings.
Vulkan reports fixed-tree and explicitly optimized full-float32 policies
separately. The optimized policy includes its right-operand layout conversion.

Warm samples include output allocation and prior-result retirement. The GPU
path uses the language runtime and TaskCore, including normal group creation,
observation, cancellation machinery and ownership checks. Resident results
remain GPU buffers; transfer-inclusive samples upload both operands and return
a host buffer. CPU and BLAS results are host buffers. Initialization and the
first call are separate measurements; this does not erase the driver's shader
cache or measure first-ever installation latency. Source compilation and
program preparation are outside warm timings. The default 64 MiB GPU byte
budget and one task worker remain in force.

Full outputs are checked outside each timed batch against double-precision BLAS
and a magnitude-dependent float32 error bound. Independent scalar dots check
the reference's layout and values. The final two outputs of every batch are
validated, not every intermediate invocation. Input fingerprints, original host-input immutability,
resource retirement, actual sample iteration counts and raw process output are
retained. Independent bit-pattern, low-precision rejection, overflow and
cancellation tests remain required: this finite matrix corpus alone cannot
prove all numerical semantics.

Acceptance runs three identical-build ABBA/BAAB control blocks per shape/input
pattern, rejects control ratios outside 0.9–1.1, then collects three forward and
reverse comparison blocks. Always audit background load and process spread;
a passing control ratio alone is insufficient. Keep failed runs and unfavorable
comparisons. Do not overlap timings with builds, tests, profiling or other
benchmark processes.

## Allocator experiments

Before rebuilding a candidate, preserve the baseline's static build inputs:

```sh
python3 native/vulkan/benchmarks/freeze_dependencies.py \
  --build build/vulkan-library-benchmark --dependency-root build/native \
  --source-root native/vulkan
```

This retains the original manifest, exact compiler/archive copies and source
snapshots, and records the relocation mapping. The executed baseline binary,
SPIR-V, adapter, scripts and live BLAS keep their hash checks. Prepare the
candidate in a new directory, run its correctness gates, then compare:

```sh
python3 native/vulkan/benchmarks/paired.py \
  --baseline build/vulkan-library-benchmark \
  --candidate build/vulkan-library-candidate \
  --output build/vulkan-library-paired
```

The allocator comparison requires byte-identical GPU kernels and CPU object
code. It alternates baseline/candidate processes in three ABBA/BAAB blocks per
case, retains load snapshots and all regressions, and verifies both manifests
before and after execution. It does not itself declare performance acceptance.
