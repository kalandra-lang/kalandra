# Apple GPU toolchain proof

This isolated proof lowers a handwritten MLIR GPU kernel through SPIR-V and SPIRV-Cross to Metal, compiles a Metal library, and executes it on an Apple M2 Max. It establishes the toolchain and buffer interface. **The original hardware shaders do not satisfy Kalandra's strict float32 contract.** A subsequent [integer lowering](../../docs/design/strict-gpu-float32.md) now passes scalar add/multiply checks; neither proof compiles Kalandra GPU source yet.

## Build and run

Provide the LLVM/MLIR release in the existing [CPU toolchain lock](../toolchain-proof/toolchain.json), the dependencies in [toolchain.json](toolchain.json), Xcode, and its Metal compiler component. The verified Homebrew packages are `spirv-cross` 1.4.363.0 and `spirv-tools` 1.4.363.0. Installation commands install whatever version the package manager currently provides; check that it matches the lock before building:

```sh
brew install spirv-cross spirv-tools
xcodebuild -downloadComponent MetalToolchain
cmake --preset native -DKALANDRA_BUILD_METAL_PROOF=ON
cmake --build --preset native --target native-metal-proof
ctest --preset native -R '^metal-proof-'
```

The proof is opt-in. The default native build has no shader-tool dependency and now has 43 tests, including external-completion coverage. Override `KALANDRA_SPIRV_CROSS_ROOT` and `KALANDRA_SPIRV_TOOLS_ROOT` for the same tool releases installed elsewhere. `KALANDRA_METAL_TOOLCHAIN` selects the Apple toolchain identifier; its compiler version is checked at configuration. The verified compiler is Apple Metal 32023.864, component 17C7003j, with macOS SDK 26.2 on macOS 26.5. The explicit toolchain selector avoids relying on the default `xcrun metal` shim finding an installed component.

Configuration resolves the compiler once, verifies its exact version and uses that
executable for proof shaders, source applications and installed compiler builds.
If Xcode resolves a different version despite the toolchain selector, supply
`-DKALANDRA_METAL_COMPILER=/path/to/Metal.xctoolchain/usr/bin/metal`. This override
must still match the lock. The selected macOS SDK is passed explicitly.

After a macOS upgrade, restore a missing component with
`xcodebuild -downloadComponent MetalToolchain`, then refresh Xcode's launcher cache
with `xcrun -k`. Check the pinned compiler using
`xcrun --toolchain com.apple.dt.toolchain.Metal.32023.864 metal --version`.
If newer Command Line Tools provide the SDK used by Homebrew Clang while the
selected Xcode has an older linker, a link may fail parsing an SDK `.tbd` file.
Use a matching linker for the build and compiler-invoking tests, for example
`export PATH=/Library/Developer/CommandLineTools/usr/bin:$PATH` when that installation
matches the SDK. This does not change the pinned LLVM or Metal compiler versions.

Execution requires access to a real Metal device. A sandbox that hides GPU services reports “no Metal device available”; this is a failure, not a skipped test or CPU fallback. Build products, intermediate IR, shader sources, reflection, version information, and per-run JSON reports live under ignored `build/native/native/metal-proof/`.

## Pipeline

The [CMake target](CMakeLists.txt) runs:

1. `mlir-opt --verify-each` with `convert-gpu-to-spirv`, then `spirv-lower-abi-attrs` and `spirv-update-vce` nested under the converted SPIR-V module.
2. A small [C++23 extraction tool](Extract.cpp). GPUToSPIRV retains the original GPU module; the tool verifies the IR and extracts exactly one SPIR-V module through MLIR's API. Missing/multiple modules and malformed input are rejected.
3. `mlir-translate --no-implicit-module --serialize-spirv`, then `spirv-val --target-env vulkan1.0` and `spirv-dis`.
4. `spirv-cross --msl --msl-version 20100` and JSON reflection. Vulkan here specifies a validation environment for the intermediate binary; no Vulkan runtime or MoltenVK is used.
5. `xcrun --toolchain … metal -std=macos-metal2.1 -fno-fast-math -ffp-contract=off -c`, then the same compiler driver links the AIR object into a `.metallib`.
6. An Objective-C++ harness loads that library through Metal and dispatches the generated `multiply_add` entry point. C++ remains the compiler implementation language; Objective-C++ is confined to the Apple API bridge.

This follows the documented [MLIR GPU](https://mlir.llvm.org/docs/Dialects/GPU/), [GPU-to-SPIR-V conversion](https://mlir.llvm.org/docs/Dialects/SPIR-V/), and [SPIRV-Cross Metal translation](https://github.com/KhronosGroup/SPIRV-Cross) paths. The fixture starts at GPU IR, so tensor-to-GPU scheduling and Kalandra source lowering are not established by this proof.

## Workload and checks

The fixture computes `out[i] = round32(round32(a[i] * b[i]) + a[i])`. Three fixed-capacity 4,096-element float32 buffers and a uint32 length occupy bindings 0–3. It uses 64-thread workgroups with an explicit active-length guard. Host admission bounds the trusted test length; arbitrary foreign descriptors are outside this proof.

The harness uses private GPU input/output buffers, shared upload/readback buffers, explicit blit uploads and downloads, and completion callbacks. Input buffers are downloaded and compared too. Every buffer has a 256-byte prefix and suffix; inactive output elements retain their sentinel. The zero-length case submits only copies and completion, without a compute dispatch. Unified memory does not erase synchronization or logical residency.

Each run covers 20 lengths from zero to 4,096, including workgroup boundaries and partial groups, three data modes, and two repetitions: **120 cases, 120 command submissions, and 76,236 element comparisons**. Normal finite values and a separately rounded multiplication/addition discriminator must match bitwise. IEEE-edge probes include subnormals, signed zero, infinities, NaNs, overflow, and cancellation near the normal/subnormal boundary. NaN payload equality is not required. The corpus contains 35,330 values for which fused multiply-add differs from the separate CPU reference.

Two shader variants distinguish numerical behavior:

- **Decorated:** the extractor attaches `NoContraction` to SPIR-V multiplication and addition. SPIRV-Cross generates `optnone` helpers using `fma(a,b,0)` and `fma(1,a,b)`.
- **Compiler rounding diagnostic:** no such decorations are added; the same Metal compiler disables fast math and contraction. This isolates the helpers' effect. It is not a change to Kalandra's numeric policy or a general proof that every future shader transformation preserves rounding.

Eight CTest tests cover both shaders with debug/optimized/ASan+UBSan host harnesses, artifact/interface validation, and extractor rejection. Metal API validation is enabled during GPU tests. ASan/UBSan instrument host code, **not GPU accesses**; shader bounds evidence comes from dispatched tail cases and full-buffer sentinel checks. ARC and per-case autorelease pools release host-side Metal objects after completion; this is not a measured GPU-memory leak or allocation-failure conformance test. Scheduler integration, cancellation, concurrent commands, production resource budgets, device loss recovery, and performance remain subsequent work.

## Result and numerical gate

All eight Metal proof tests pass on the recorded M2 Max. At the original shader-proof milestone, the full optional build passed all 47 tests (39 existing native tests plus these eight), including the existing separate ThreadSanitizer suites. Both variants pass normal finite, separate-rounding, input-preservation, and guard checks. IEEE edge differences are **observations recorded by passing toolchain probes**, not numerical conformance successes:

| Shader                       | IEEE edge mismatches per run | Strict float32 sample |
| ---------------------------- | ---------------------------: | --------------------- |
| Decorated                    |                       10,662 | Fails                 |
| Compiler rounding diagnostic |                        8,542 | Fails                 |

For example, `a` with float bits `0x00000001` and `b = 1` should yield `0x00000002`; both GPU variants return zero. Negative subnormals and a cancellation case also differ. The decorated path additionally turns the `a = -0, b = 1` result from negative zero into positive zero. The compiler-controlled path preserves that case, so attributing every mismatch to Apple hardware would be incorrect.

Apple's [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf), sections 8.1 and 8.5 (June 4, 2026 edition), describe permitted floating-point behavior, including subnormal flushing. Disabling fast math alone is insufficient to establish Kalandra's stricter contract. Passing a finite corpus would also be insufficient to establish general IEEE compatibility.

The [recorded reports and artifact hashes](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/2026-10-15.json) retain these results. This proof is **not enabled as a production float32 GPU backend**. Admission for strict arithmetic must remain unavailable until a compliant lowering exists; neither silent CPU fallback nor silently relaxed arithmetic is selected. See the [Apple GPU backend contract](../../docs/design/apple-gpu.md) for integration gates.

## Strict binary32 follow-up

The optional proof build now also produces `native-metal-strict`: an integer MLIR lowering for scalar float32 add/multiply, a CPU LLVM reference fixture, and a Metal artifact. Eight additional tests compare it with LLVM APFloat and CPU hardware, verify generated arithmetic/interfaces, and reject unsupported operations. The strict shader requires MSL 2.2/Int64; the original hardware controls above retain their MSL 2.1 profile and recorded mismatches. See the [implementation and results](../../docs/design/strict-gpu-float32.md). No arithmetic policy is relaxed and no performance claim is made.

The strict lowering now uses five bounded integer normalization stages instead of per-bit loops. The current scalar proof corpus also covers every subnormal leading-bit position and cancellation distance across all finite exponent fields; historical reports retain the earlier corpus. Artifact checks reject normalization loops in the scalar fixtures. See the [optimization and qualification](../../docs/design/apple-gpu.md#strict-normalization-optimization).

The subtraction milestone extended the scalar proof with subtraction and exact sign-bit negation alongside add, multiply and separately rounded multiply/add. That five-mode qualification checked 2,975,530 CPU comparisons and 3,039,060 GPU comparisons per corresponding variant, including exact NaN payloads for negation. The subsequent division proof adds a sixth mode and directed quotient cases. The source compiler admits tensor subtraction and scalar negation through the same lowering. See [source semantics and qualification](../../docs/design/strict-gpu-float32.md#source-subtraction-and-scalar-negation).

[Strict float32 division](../../docs/design/apple-gpu.md#strict-tensor-division) now supports tensor/tensor and either scalar/tensor order, including transitive scalar reciprocal helpers. Division retains separately rounded results rather than becoming multiplication by a reciprocal.

## All-type numerical foundations

The typed source backend adds checked integer arithmetic for all eight integer
widths, strict integer-only binary64 arithmetic and checked conversions for all
100 numeric type pairs. `IntegerOracle.h`, `Strict64Gpu.mm` and
`ConversionOracle.h` compare actual shader results against LLVM APInt/APFloat;
CPU compact-lowering tests and shader artifact checks qualify the same integer
algorithms independently. Typed shaders require the pinned MSL 2.3 profile.
`NumericWire.cpp` also emits typed map/scalar/reduction fixtures used by the
managed runtime's failure, cancellation and ownership tests. These foundations
are integrated into the [typed source programs](../../docs/design/native-gpu-typed-programs.md).
