# Linux CUDA development bring-up

This opt-in implementation is under development on `codex/cuda-linux` (#264).
The opt-in development compiler accepts `--target cuda`; installed/release delivery is still under qualification. macOS remains Metal-only.

The device compiler lowers the existing typed GPU IR directly through NVVM to
PTX. Hardware arithmetic retains native float32/float64 instructions. Software
arithmetic is explicit; the optional software companion is admitted only after
an unsupported-capability result, never after malformed code or a device fault.
There is no numerical CPU fallback, implicit TF32, or production cuBLAS dependency.

The runtime loads the system NVIDIA Driver API lazily, creates private contexts,
restores the calling thread's previous context, and validates descriptor metadata
against driver parameter reflection. Task completion runs on the existing foreign
completion service. Cancellation is acknowledged between completed stages; it
never resets the GPU or interrupts another process's work. Owned buffers and
modules retain their context until cleanup. Device allocations request their logical extent, with conservative quotas rounded
to reported allocation granularity, and share a byte ledger with retained host data;
module count has an independent bound. Context/JIT/driver-internal memory is
opaque and is not represented as a tensor-byte allocation. Synchronous raw
`downloadBytes` returns caller-owned host memory; retained language snapshots use
the shared host-byte ledger. The private artifact checksum detects corruption;
it is not authentication or a sandbox for user-written GPU kernels.

With already installed CUDA development headers, CMake 3.28+, and a C++23 compiler:

```sh
cmake -S native/cuda -B build/cuda-runtime -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cuda-runtime --parallel 4
```

The SDK is a build dependency. Applications link only the project runtime and
load `libcuda.so.1`; NVCC, NVRTC, cuBLAS and SDK libraries are not runtime imports.
LLVM's device compiler avoids NVCC host-compiler restrictions. The Linux
qualification SDK currently supplies CUDA 13.2 parameter reflection; older driver
compatibility is not yet qualified. PTX baseline is SM80, with SM120 also tested.

Root builds use `KALANDRA_BUILD_CUDA_SOURCE=ON` and
`KALANDRA_BUILD_CUDA_RUNTIME=ON`. Hardware tests are explicit, never run merely by
configuring a CPU compiler. Generate compiler-owned arithmetic artifacts, then
run the bounded raw suite on an available NVIDIA GPU:

```sh
python3 native/cuda-source/qualification/generate.py \
  --compiler build/native/native/cuda-source/kalandra-cuda-lower \
  --output build/cuda-artifacts
build/cuda-runtime/kalandra-cuda-runtime-tests build/cuda-artifacts
```

Build a complete development application with the same compiler command:

```sh
cmake --build build/native --target kalandra --parallel 4
build/native/native/kalandra --target cuda -O2 example.k -o build/cuda-example
build/cuda-example/program
```

The compiler also accepts project manifests/local packages, all three arithmetic
policies, and `-O0`/`-O2`. GPU cross-compilation and the public `--sanitize` /
`--thread-sanitize` delivery variants are not qualified. The CPU compiler and
other enabled GPU targets remain available in the same executable. No CUDA
system software is installed by the compiler.

Matmul defaults to `fixed-tree`. Explicit `--matmul-policy optimized` uses the
pinned MIT full-float32 warp-tiled kernel for supported aligned float32 shapes.
Its FMA/accumulation order differs from the adjacent-pair fixed tree; it never
selects TF32 or reduced-precision input storage. Other shapes, zero dimensions,
odd K, and the disabled tiled capability use the bundled fixed-tree **GPU**
companion. Software arithmetic always retains fixed-tree products. CUDA consumes
B in row-major `[K,N]` layout, without an extra transpose stage. The matrix-layout
trait leaves Vulkan's existing imported-kernel layout unchanged.

Applications deploy as a directory: keep `program`, every `.ptx` file (including
`.fixed-tree.ptx` / `.software.ptx` companions), and `THIRD_PARTY_NOTICES.txt`
together. Kernel paths resolve relative to the actual executable, including PATH
and symlink launches. Native matrix artifacts have an integrity-covered V2 tile
contract; ordinary NVVM kernels retain the V1 descriptor contract. This is a
private compiler/runtime ABI, not the user-written-kernel interface.

The private `native/cuda-source/qualification/build_application.py` helper
remains available for isolated qualification. Numerical/conversion, structured
ownership, packaging/relocation, CI and honest kernel/end-to-end performance
have separate gates in `docs/native-cuda-roadmap.md`. Correctness runs do not
constitute performance evidence.

CUDA host sanitizer builds use `KALANDRA_CUDA_SANITIZERS=ON`. On the qualified
Linux driver, ASan's protected shadow gap prevents `cuInit` from reserving its
address space (CUDA error 2). Run these instrumented test executables with the
process-local setting `ASAN_OPTIONS=protect_shadow_gap=0`, as documented by
[NVIDIA](https://developer.nvidia.com/docs/drive/drive-os/archives/6.0.4/tensorrt/pdf/NVIDIA_DRIVE_OS_6.0_TensorRT_8.4.11_Developer_Guide.pdf).
Address/undefined/leak checks remain enabled; this is not a system setting or a
production runtime default. GPU memory is checked separately with Compute Sanitizer.

Transpose reuses the project-owned bit-copy algorithm for all ten types. The
float32 row primitives port the existing adjacent-pair softmax/RMS algorithms,
including all barriers, NaN/infinity rules and checked epsilon validation.
Build-time CUDA `expf`/`sqrtf` compiler primitives supply the standard scalar
math; NVRTC runs with FMA contraction and flush-to-zero disabled for these
resources. Applications receive resolved PTX and require no NVRTC/math-library
runtime dependency. CUDA row tests use the CPU/Metal rounding oracle, including
its four-ULP exp interval, rather than Vulkan's wider hardware bounds.

## Initial performance qualification

Two frozen RTX 5090 series on 2026-11-01 compared the production-descriptor
64/128 tile kernels with cuBLAS 13.4.1 in `CUBLAS_COMPUTE_32F_PEDANTIC` mode.
Four ABBA blocks per pair and identical-kernel controls passed the predeclared
5% gate in both series. Every timed output and separate nonuniform, sub-TF32
precision and prior-NaN controls passed. The 64 tile was consistently faster,
so it is the installed optimized resource; the 128 tile remains a candidate.

| M×N×K | 128 tile / 64 tile time | 64 tile / cuBLAS time (approximately) |
| --- | ---: | ---: |
| 512×512×512 | 1.72 | 3.55 |
| 1024×1024×1024 | 1.42 | 2.14 |
| 2048×2048×2048 | 1.18 | 1.51 |
| 512×1024×768 | 1.72 | 1.89 |

These are resident prepared-launch GPU-event intervals, including submission
gaps. They exclude compilation, allocation, transfers and language scheduling;
they are **not** end-to-end language measurements or a claim of vendor parity.
cuBLAS is reference-only. `benchmarks/ResidentMatmul.cpp` and `run_resident.py`
retain the protocol, full output checks, frozen hashes and failed-control
handling. Raw machine-specific series belong in qualification artifacts, not
large checked-in reports. No clock/driver settings were changed.

The production-pinned `native-cuda-standalone-package` target stages an owned,
relocatable compiler. Its exact hosted artifact, missing-resource behavior and
physical execution still require qualification before release. The development
LLVM 23.1.4 profile continues to disable install rules; it cannot satisfy that gate.

Whole-application latency is a different measurement. `benchmarks/program_latency.py`
separates preparation from execution, hashes every generated application, checks
all result cells, and includes process/context setup, module/JIT-cache work,
transfers, scheduling, output formatting and cleanup. It leaves the driver cache
untouched and does not call its first observation a cold JIT. An initial
LLVM 23.1.4 development series completed all 68 program executions correctly,
but five of eight identical-program controls exceeded the predeclared 5% gate.
Those samples are retained; there is no accepted end-to-end speedup claim.

`qualification/float_conformance.py` under `native/cuda-source` generates four
basic operations for float32/float64 and all three arithmetic policies. The
explicit hardware test compares 916,272 outputs with LLVM APFloat, bit-exact for
non-NaNs (including subnormals and signed zero), NaN classification for hardware,
and canonical NaNs for software. This oracle is test-only and adds no LLVM
requirement to the standalone runtime or generated programs.
