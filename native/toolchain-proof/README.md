# Native toolchain proof

The current toolchain is Homebrew LLVM/MLIR/Clang **23.1.3**, CMake **4.4.2**, and Ninja **1.13.2**, on macOS ARM64. Compiler-side tools and the independent numerical reference use C++23. See the [LLVM 23 migration](../../docs/design/native-llvm23.md) for current qualification and compatibility details.

The original proof was qualified on 2026-10-15 with LLVM/MLIR/Clang **22.1.8**; historical evidence retains that version.

Run from the repository root:

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

The checked-in [toolchain lock](toolchain.json) requires matching LLVM, MLIR, and Clang versions. Tools are invoked by their full paths, so no shell profile or global PATH change is required. For the same version installed elsewhere:

```sh
cmake --preset native -DKALANDRA_LLVM_ROOT=/path/to/llvm-23.1.3
cmake --build --preset native
ctest --preset native
```

This lock validates the installed version; it does not pin the global Homebrew formula or prevent Homebrew cleanup. A fresh machine must install/provide the exact release before running the proof. A newer unversioned `brew install llvm` is not assumed compatible. The [native release workflow](../../docs/design/native-releases.md) provisions the locked major and rejects a mismatched release. Self-contained and fully reproducible dependency installation remain follow-up tooling work.

## Verified path

[projection.mlir](projection.mlir) is hand-written fixed-shape MLIR. It takes runtime matrix contents, constructs a tensor with explicitly rounded products and balanced sums, returns that tensor across a function call, and reduces it to float32.

The [CMake build](CMakeLists.txt) runs these pinned tools and passes in order, with verification at each MLIR stage. It first checks the source module through the [native C++23 MLIR API tool](../tools/mlir-check.cpp):

1. `mlir-opt --one-shot-bufferize=bufferize-function-boundaries`
2. `mlir-opt --buffer-deallocation-pipeline`
3. `mlir-opt --convert-linalg-to-loops --convert-scf-to-cf --finalize-memref-to-llvm=use-generic-functions --convert-to-llvm --reconcile-unrealized-casts`
4. `mlir-translate --mlir-to-llvmir`
5. `clang` linking the generated LLVM IR with the private C harness.

Intermediate IR, input datasets, native binaries, optimized assembly, and a machine-readable report are written under ignored `build/native/native/toolchain-proof/`. CMake is the executable specification of complete arguments and paths; CTest writes one JSON evidence report for each native build variant.

Tensor generation becomes linalg.map during bufferization, so linalg-to-loops is required before LLVM translation. Ownership lowering inserts the caller's release of the callee's returned allocation. The generic allocation hooks let the private harness count and validate allocations/releases without replacing system malloc globally.

These stages follow MLIR's separation between bufferization and ownership/deallocation, then LLVM translation. [Bufferization documentation](https://mlir.llvm.org/docs/Bufferization/), [LLVM target documentation](https://mlir.llvm.org/docs/TargetLLVMIR/).

## Evidence

The CMake build produces debug (`-O0`), optimized (`-O2 -fno-inline`), and AddressSanitizer/UndefinedBehaviorSanitizer (`-O1 -fno-inline`) executables. All use `-ffp-contract=off`; the generated LLVM IR has no fast-math flags, and optimized assembly is checked for fused multiply-add instructions.

Each build passes:

- Default execution prints exactly `163` and a newline, with Bun/LLVM directories absent from runtime PATH.
- 49 runtime-input datasets match an independent C++23 float32 reference bitwise, except that NaN payload identity is intentionally ignored.
- Cases include changed inputs, fixed-seed finite data, signed zero, the smallest subnormal, infinity, NaN, separate product rounding, odd contraction length, and balanced reduction versus sequential accumulation.
- 10,000 repeated calls produce 10,000 allocations and releases with no remaining live result buffer and no mutation of either input.
- Injected allocation failure reports AllocationFault and exits 86 before a null pointer is used.

The ordinary executable's only dynamic dependency is `/usr/lib/libSystem.B.dylib`; sanitizer builds additionally use their sanitizer runtime. No LLVM, MLIR, JavaScript engine, Bun, or Node application runtime is linked. The harness compiles cleanly under `-std=c11 -Wall -Wextra -Werror`.

The allocator is deliberately limited to one live result allocation, which this fixed-shape program requires. Counts provide direct leak evidence for those allocations; AddressSanitizer on macOS is not claimed to provide LeakSanitizer coverage. UBSan instruments the C harness, while already-lowered LLVM IR retains only the checks present in that IR plus AddressSanitizer's memory instrumentation. This does not prove that unchecked arithmetic in arbitrary generated IR would be safe.

## Scope

This completes the first toolchain gate of the [native CPU milestone](../../docs/design/cpu-milestone.md). It does not compile `.k` source yet, support arbitrary shapes, provide a general tensor ABI, or implement the full language runtime.

The fixed descriptors are constructed by trusted test code and their input storage is independently owned. Incoming malformed descriptors, general dynamic shape validation, overflow-safe dynamic allocation, multiple live allocation failure cleanup, closures, task supervision, GPU/remote residency, and the future memory-management mechanism remain subsequent implementation work.

The five CTest tests include three numerical/ownership suites and two direct MLIR API checks: valid module acceptance and invalid return-type rejection. The former Bun verifier has been removed.

The next gate is an isolated fresh C++23 frontend with the new grammar, source spans, and explicit diagnostics. The hand-written MLIR becomes a backend reference fixture rather than being presented as compiler output.
