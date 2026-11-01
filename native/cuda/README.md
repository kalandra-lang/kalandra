# Experimental Linux CUDA backend

This opt-in implementation is under development on `codex/cuda-linux` (#264).
The opt-in compiler accepts `--target cuda`. Downloaded production-pinned Linux packages have been executed on the RTX 5090; future publication is gated by reproducible composition and relocated package qualification. CUDA is not in v0.99.1. macOS remains Metal-only.

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
pinned BSD-3-Clause CUTLASS C++ full-float32 SIMT kernels for supported aligned
float32 shapes. The build downloads only the pinned C++ include tree and license
into a build-local cache; Python/CuTeDSL's separate license is excluded. Offline
builds can set `KALANDRA_CUTLASS_ARCHIVE` to the hash-verified source archive.
Its FMA/accumulation order differs from the adjacent-pair fixed tree; it never
selects TF32 or reduced-precision input storage. Other shapes, zero dimensions,
odd K, and the disabled tiled capability use the bundled fixed-tree **GPU**
companion. Software arithmetic always retains fixed-tree products. CUDA consumes
B in row-major `[K,N]` layout, without an extra transpose stage. The matrix-layout
trait leaves Vulkan's existing imported-kernel layout unchanged.

Applications deploy as a directory: keep `program`, every `.ptx` file (including
`.fixed-tree.ptx` / `.software.ptx` companions), and `THIRD_PARTY_NOTICES.txt`
together. Kernel paths resolve relative to the actual executable, including PATH
and symlink launches. Legacy native matrix artifacts have an integrity-covered V2 tile
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

## Optimized matrix catalog

The private V3 artifact admits four reflected descriptor functions in one module:
a 64×128 tile (128 threads), a 128×256 tile (256 threads), a 256-thread reduction,
and a 256-thread scratch-free fallback. Legacy V1/V2 artifacts remain supported.
Every function has the same twenty 64-bit descriptor arguments. The bounded
catalog does not introduce public user kernels, module-cache growth or a new
source ABI.

Both tiled entries split K into five independent, eight-element-padded slices.
They use float32 SIMT FMA and a fixed balanced eight-leaf reduction (unused leaves
are zero). There are no floating atomics or inter-CTA waits. Products retain the
previous aligned accelerated domain; tails, empty tensors, K=0, disabled tiled
capability and unsupported geometry use the fixed-tree GPU companion. The
optimized policy already permits FMA, reassociation and intermediate-overflow
behavior differing from a serial or fixed-tree dot product.

Optional scratch is five float32 output planes, charged at the device allocation
granularity. When full planes exceed the quota, the runtime reuses 512 or 256
output rows of scratch with a separately budgeted per-band shape table. Every
band preserves the same K slices and reduction order. If even that cannot fit,
the scratch-free route is used. Scratch is fully overwritten before use. Partial and reduction launches
share one context lock, stream and final completion fence. Failures in either
launch still settle before resources retire. OOM/quota rejection before launch
selects the scratch-free function, which computes the same partitions, padding,
FMA order and reduction tree. Device errors propagate; they never trigger this
fallback. Scratch is released after completion and before status-readback
staging. Cancellation/deadline acknowledgement stays at the completed source
primitive; the two private launches remain one language stage.

`tests/CatalogTests.cpp` covers independent numerical oracles, full-FP32 low-bit
and subnormal witnesses, bit-identical workspace fallback, real tight quotas,
module admission, foreign ownership, narrow/wide bands and final row tails, partial/reducer failure settlement and
cancellation/deadline acknowledgement with 1/2/4 workers. Compute Sanitizer and
host ASan/UBSan exercise the actual runtime, not only a kernel launch prototype.

`benchmarks/RuntimeMatmul.cpp` separately measures actual runtime invocations
against the legacy kernel at the unchanged default 64 MiB budget and an explicit
256 MiB ceiling. It includes output/parameter/workspace allocation, completion,
status readback and cleanup with resident inputs; task scheduling, process startup
and input transfer remain outside that scope. Do not confuse its wall times with
the prepared resident GPU-event numbers below.

## Resident performance evidence

Four frozen idle RTX 5090 runs on 2026-11-01 (two repetitions per tile orientation)
passed complete-output correctness and all 768 identical-kernel controls at the
predeclared 5% threshold. Twelve shapes included eight holdouts. Best tested
mirrored split-K kernels compared with the earlier tile64 baseline and cuBLAS
13.4.1 `CUBLAS_COMPUTE_32F_PEDANTIC` as follows:

| M×N×K | Time / tile64 baseline | Time / cuBLAS |
| --- | ---: | ---: |
| 512×512×512 | 0.246 | 0.885 |
| 1024×1024×1024 | 0.434 | 0.934 |
| 1536×1536×1536 | 0.824 | 1.254 |
| 2048×2048×2048 | 0.772 | 1.176 |
| 512×1024×768 | 0.358 | 0.677 |

Lower is faster. These are prepared resident prototype kernel intervals,
including both launches and submission gaps, excluding compilation, allocation,
transfers and task scheduling. They are not end-to-end compiler results. The
wide tile regresses on the 64³ holdout, so it is not selected universally. Large
1536³/2048³ products still trail cuBLAS. Earlier shared-desktop series failed
controls and remain separate; they are not pooled into the accepted results.

`benchmarks/CatalogResident.cpp` loads the production V3 and legacy V2 resources
for repeatable reference-only comparisons. It is never linked into applications.
Build it with the existing CUDA SDK (`clang++ -std=c++20 -O2`, CUDA include/lib
paths, `-lcublas -ldl`). Its arguments are catalog PTX, output JSONL, and legacy
warp64 PTX. It validates all output cells before timing and retains four ABBA
blocks per pair, independent controls and eight holdouts. Raw machine-specific
series belong in qualification artifacts, not large checked-in reports.

The production-pinned `native-cuda-standalone-package` target stages an owned,
relocatable compiler. The downloaded hosted artifact passed relocation, 153 GPU executions, eight
missing-resource checks, independent numerical oracles and GPU memory checks.
Release workflows separately require exact-tag provenance and qualification of
the combined CPU-target/CUDA archive before publication. The development
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
