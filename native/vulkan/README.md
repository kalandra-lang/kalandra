# Experimental Vulkan runtime bring-up

This is an internal backend implementation, not a released Vulkan compiler target.
The source-to-device compiler and structured task integration are still pending:
see [implementation plan](../../docs/native-vulkan-roadmap.md) and #263.

The opt-in root build flag is `KALANDRA_BUILD_VULKAN_RUNTIME`. To qualify just the
runtime with existing Vulkan development tools, without installing LLVM/MLIR:

```sh
cmake -S native/vulkan -B build/vulkan -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vulkan --parallel 2
ctest --test-dir build/vulkan --output-on-failure
# Test another enumerated hardware GPU explicitly:
build/vulkan/kalandra-vulkan-tests 1
```

Requires Vulkan headers/loader, glslc, spirv-val, spirv-dis, CMake 3.28+, a C++23
compiler/standard library, Python 3 for the provenance audit, and the Khronos
validation layer for hardware tests. No dependency downloads or installation
commands run during configure/build. System software on the user's Linux host
must be installed only by the user or with their explicit approval.

## Kernel provenance and numerical scope

The two built kernels are unmodified upstream `add.comp` and `mul_mm.comp` from
llama.cpp revision `a91eab67d8389d979d29fdf6633b2edd0718eb4b` (MIT). Their
transitive textual includes are retained unmodified. [UPSTREAM.json](third_party/ggml/UPSTREAM.json)
records SHA-256 hashes and [LICENSE](third_party/ggml/LICENSE) retains notices.
The imported source set contains conditional quantization, expert-routing and
cooperative-matrix code, but these variants are not enabled or claimed supported.
No Steel/CUTLASS/CuTe code or unrelated shader library is imported here.

Build definitions select float32 inputs, outputs and accumulators; the tiled
matrix kernel uses ordinary float32 arithmetic, not cooperative-matrix TF32/FP16.
Both actual SPIR-V outputs must validate and contain only the Shader capability
and 32-bit floating-point types. A runtime one-product case detects reduced
operand precision. Matrix tests compare every output against independent double
references with a magnitude-based float32 error bound. This does not establish
bitwise equivalence with Metal's historical fixed-tree policy, nor qualification
of NaN/denormal/signed-zero behavior. Those policies must be handled explicitly
before source-language integration.

## Ownership and bring-up limits

Buffers own their allocation and retain the device independently of the runtime
facade. Upload copies host data; inputs remain immutable; foreign contexts and
invalid shapes are rejected. The quota covers actual live Vulkan allocation
sizes, including alignment. Calls are serialized per runtime, with one command
in flight; fences settle submitted access before descriptor/command/buffer
resources are released. Noncoherent mappings are flushed/invalidated explicitly.
Software devices are excluded rather than used as a silent GPU fallback.

This initial path uses host-visible storage on discrete GPUs, synchronous fence
waits and an absolute build-tree shader directory. It is not a performance or
relocatable-distribution implementation. Device-local staging, async task
completion/cancellation/deadlines, injected failures, full type/rank support,
capability dispatch, fixed-tree policy, user-facing diagnostics and packaging
remain required before the backend can be considered ready. The explicit
right-transposed matmul interface is a private layout contract, not new language
syntax. Empty outputs and zero-contraction products perform no GPU dispatch.

See the dated qualification entry in the implementation plan for actual tested
hardware and unresolved sanitizer findings. Do not use wall-clock test duration
as a kernel benchmark, and coordinate performance runs with the other GPU user.
