# Kalandra native compiler and runtime

Kalandra uses a C++23 frontend, MLIR/LLVM CPU lowering and Apple Metal GPU
execution. It supports explicit CPU/GPU placement and transfers, immutable typed
values and structured tasks. The [language design](../docs/design/core.md),
[GPU capability matrix](../docs/design/native-gpu-capabilities.md) and
[native roadmap](../docs/roadmap.md) state the supported subset and remaining work.

## Start a project

With an installed mixed CPU/Metal compiler:

```sh
kalandra new my-app
kalandra-check --project my-app/kalandra.toml
kalandra --project my-app/kalandra.toml -o my-app/program
./my-app/program
```

Use `--target metal` for a supported GPU program. See the
[starter guide](../docs/design/native-starter.md),
[CPU installation](../docs/design/native-cpu-install.md) and
[Metal installation](../docs/design/native-metal-install.md).

## Build and test

```sh
cmake --preset native
cmake --build --preset native --parallel 2
ctest --preset native
```

CMake 3.28+, Ninja, toml++ 3.4.0 and the pinned
[LLVM/MLIR/Clang toolchain](toolchain-proof/toolchain.json) are required. Metal builds
also require the pinned [Apple toolchain](metal-proof/toolchain.json). Follow the
[platform qualification guide](../docs/design/native-distribution-qualification.md)
for source builds and standalone packaging. Release targets are macOS 26+ ARM64
CPU/Metal, Linux x86-64/ARM64 CPU and Windows x86-64/ARM64 CPU. Windows TSan and Windows ARM64 sanitizers are unsupported.

Experimental Linux Vulkan builds use `KALANDRA_BUILD_VULKAN_RUNTIME=ON` and
`--target vulkan`, with Vulkan headers, glslc and SPIR-V tools available at build
time. The same numerical placement/transfer APIs are shared with Metal. Vulkan
is not included in v0.99.1 release assets; candidate package and physical-device
qualification are documented in the [Vulkan source guide](vulkan/README.md).

Starting with 0.99.1, standalone compiler packages contain all five CPU runtimes
and private host-native linkers. Select a destination using `--platform`, or
configure its system libraries with a reusable `--toolchain` profile. Linux and
Windows target development libraries remain separate; macOS CPU cross-linking
uses bundled system import declarations without an Apple SDK. See the
[cross-compilation guide](https://kalandra.dev/docs/cross-compilation).

CTest covers frontend/lowering, numerical and ownership semantics, recursive
calls, workers, cancellation/fault precedence, GPU capabilities, editor tools and
relocated distributions. Sanitizer modes are separate build/test variants.
The CMake files are the executable source of test commands and resource locks;
historical test counts are not current qualification claims.

## Numerical and execution policies

Hardware arithmetic is the default where supported. `--arithmetic software` and
`--arithmetic hardware-with-software-fallback` make fallback explicit. Optimized
GPU float32 matmul permits full-precision FMA/reordered accumulation;
`--matmul-policy fixed-tree` preserves the earlier contraction order. Software
arithmetic forces fixed-tree. CPU matmul currently keeps its existing tree under
both flags. [Numerical contracts](../docs/design/hardware-arithmetic.md).

Production has no Accelerate/MPSGraph dependency. The qualified M2 Max profile
uses the [pinned MIT Steel subset](third_party/mlx-steel/README.md); other devices,
unsupported shapes and fixed-tree execution retain native fallback kernels.
Native host clients must rebuild matching headers and static archives when the
internal C++ interface changes.

## Benchmarks and implementation conventions

[Supported benchmark runners](benchmarks/README.md) separate source-level language
comparisons, numerical libraries and full models. [Current evidence](../benchmarks/README.md)
discloses failed controls and remaining performance uncertainty. Retired research
reports and prototypes are archived, not part of the production runtime.

- Use C++23 with compiler extensions disabled. Add standard-library features only after validating them with the supported compiler and platform library.
- Prefer values, RAII, `std::unique_ptr` for unique ownership, and stable source-buffer ownership for any borrowed source spans. Raw pointers/references express documented non-owning access, not hidden ownership.
- Use `std::expected` for recoverable frontend/IO diagnostics. Treat invalid source as a reported error, not an assertion failure. Unexpected process-level allocation failure needs a separate top-level policy before the compiler accepts arbitrary inputs.
- Keep source locations, AST, resolution/type checking, and lowering separate. Build a focused lexer/parser for the fresh syntax; do not reproduce the prototype's parser ambiguities.
- Keep MLIR contexts/modules under their owning API wrappers, verify constructed IR, and preserve shape, failure, and numerical semantics through lowering.
- Keep templates and abstractions small and useful. C++ implementation complexity should not become language syntax or user-facing memory management.
- Test behavior and diagnostics through CTest. Use sanitizers for relevant native memory paths, while keeping their coverage limits explicit.

Compiler implementation ownership is separate from Kalandra's automatic memory model. Using RAII inside the compiler does not require Kalandra authors to write destructors, allocators, or lifetime annotations.
