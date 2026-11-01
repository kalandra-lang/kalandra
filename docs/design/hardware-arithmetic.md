# Hardware arithmetic and explicit software fallback

Current policy, 2026-10-24. This supersedes statements in historical qualification reports that describe software arithmetic as the ordinary source compiler's default.

The default compiler policy is **hardware**. CPU float32 and float64 use LLVM floating-point operations. CPU floating comparisons and conversions use floating instructions, with validity guards retaining the checked conversion contract. Apple Metal float32 uses floating operations in the generated shader. Checked integer arithmetic still uses native integer operations and overflow/range checks; widened intermediates and partial-product sequences do not imply floating-point emulation.

| Policy | CPU float32/float64 | Metal float32 | Metal float64 arithmetic |
| --- | --- | --- | --- |
| `hardware` (default) | Hardware | Hardware | Rejected before output publication |
| `hardware-with-software-fallback` | Hardware | Hardware | Explicit strict software fallback |
| `software` | Strict software | Strict software | Strict software |

```sh
kalandra main.k -O2 -o app
kalandra main.k --target metal -O2 -o app
# Explicitly permit float64 arithmetic on a Metal target without native FP64:
kalandra main.k --target metal --arithmetic hardware-with-software-fallback -O2 -o app
# Select the prior integer-based strict IEEE arithmetic implementation:
kalandra main.k --arithmetic software -O2 -o app
```

The policy propagates to ordinary functions, CPU workers, CPU exports in mixed programs, and all generated numerical graph stages. Metal bundles record `arithmetic=...` in `manifest.txt`. Hardware mode rejects unsupported float64 GPU arithmetic, comparisons and conversions with an actionable diagnostic. Merely transporting float64 data does not authorize emulation or require floating-point computation. A GPU fallback still runs integer instructions **on the GPU**; there is no implicit CPU fallback.

## Experimental Vulkan target

Vulkan uses the same explicit arithmetic policies. Native float64 instructions
are retained on supported devices, with signed-zero/infinity/NaN preservation
and advertised denormal controls requested at load time. RTX 5090 passes the
native four-operation edge corpus. Intel native float64 division currently fails
that contract, so hardware-only loading returns Unsupported (106); explicit
`hardware-with-software-fallback` may select the integer GPU companion. Other
Intel float64 arithmetic remains native. This is a conservative Intel restriction
based on the tested ARL driver, not universal driver qualification.

A missing or malformed primary shader never triggers numerical fallback. Vulkan
currently defaults to fixed-tree matmul; `--matmul-policy optimized` explicitly
selects the full-float32 tiled algorithm and its extra GPU layout workspace.
Software arithmetic retains fixed-tree products. This target is experimental and
is not part of the v0.99.1 release assets. See the
[Vulkan source guide](../../native/vulkan/README.md) for device and package limits.

## Numerical contract

Hardware mode preserves numeric types, checked integers, shape checks and checked
conversion admission. Scalar/elementwise arithmetic and ReduceSum retain their
existing separately emitted operations and reduction order. `-O2` does not turn
on generic fast math or scalar FMA contraction. Metal hardware float32 **matmul** uses
the optimized full-precision tensor policy by default: fused accumulation and
implementation-dependent contraction order are allowed without dropping input
precision. See [tensor numerical policies](tensors.md#precision-reductions-and-optimization).
Use `--matmul-policy fixed-tree` to retain separately rounded matrix products
and the balanced contraction tree. CPU matmul currently uses that tree under
both policies; integer matmul and all checked faults are unchanged.

Hardware mode does **not** promise bit-identical floating results across every
host floating environment and GPU. Hardware rounding/underflow/NaN behavior
applies: Metal float32 subnormals may flush to zero, and hardware casts/comparisons
may affect floating exception flags. Optimized matrix accumulation can additionally
change signed zero and intermediate-overflow behavior. No ambient floating
environment is changed by generated code. Explicit software mode preserves the
previous host-environment-independent, gradual-underflow, canonical-arithmetic-NaN
contract and always selects fixed-tree matmul, regardless of the hardware default.

Floating casts retain the existing bounds rules. Invalid NaN/infinity/out-of-range float-to-integer inputs are rejected **before** executing a hardware conversion; they never reach undefined LLVM `fptosi`/`fptoui` behavior. Float narrowing retains finite-overflow checks. Integer-to-integer conversions remain checked integer operations. Floating identity conversions retain their input value.

Software mode outlines CPU floating operations into shared integer helpers, avoiding unbounded caller code/stack growth. Software float32 matrix multiplication takes the scalar arithmetic path rather than leaving hardware SIMD operations in the program. Metal software arithmetic uses the existing qualified integer shader lowering. Original strict conformance fixtures now request software mode explicitly; historical evidence files remain unchanged.

## Validation and benchmarks

`native/tests/ArithmeticPolicyTests.py` checks defaults and every policy at both CPU optimization levels, CPU workers, checked casts and faults, comparisons, invalid CLI options, real Metal execution, transactional unsupported-operation rejection, and mixed graphs. It inspects generated LLVM/MLIR to distinguish hardware instructions from integer emulation, including a software float32 matrix case. The mixed test verifies hardware CPU float64 and GPU float32 while **only** GPU float64 falls back.

The [language benchmarks](../../native/benchmarks/languages/README.md) record the selected type/policy and assert generated arithmetic artifacts before collecting timings. The default is a float32 hardware suite; Python retains its ordinary binary64 floats and the chosen inputs/intermediates are exactly representable in both formats. A separate float64 run explicitly permits Metal software fallback while retaining hardware CPU float64. Old all-software float64 measurements remain a historical baseline, not current hardware performance evidence.

## Recorded hardware-policy qualification, 2026-10-24

The subsequent [CPU optimization qualification](cpu-optimization.md) records the current compiler performance; the measurements below retain the intermediate hardware-policy baseline.

The complete native build passed. Across the broad numerical run and targeted reruns, 90 distinct native numerical/conformance tests passed, including debug, optimized, address/undefined and thread sanitizer variants, strict software proofs, hardware GPU casts across all eight integer types, checked cast failures, and CPU/GPU helper comparisons. The docs verification also passed all 39 extracted blocks (32 CPU programs, two GPU programs, four companion blocks and one intentional diagnostic).

Fresh Apple M2 Max/macOS 27.2 reports:

- [Hardware float32 CPU and Metal](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/languages-hardware-float32-2026-10-24.md).
- [Hardware CPU float64 and explicitly enabled Metal software float64](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/languages-hardware-float64-fallback-2026-10-24.md).

Each profile has 42 timing cells, 126 fresh measured processes, 882 measured batches and 252 warmup batches. All checksums, recorded source/compiler hashes and arithmetic-artifact assertions were verified. Python retains binary64 in both profiles. Raw JSON accompanies each report.

Compared with the retained earlier all-software float64 run, CPU float64 improved by 22–34× across these six local cases. For example, the 262,144-element dot product went from 15,323 to 566 microseconds, scaled addition plus sum from 17,944 to 809 microseconds, and the 96×96 matrix case from 48,505 to 1,434 microseconds. These are separate local runs, not isolated frequency-controlled measurements. GPU dispatch/transfers still dominate several small cases, and remaining CPU allocation/loop costs are visible relative to the other compiled implementations.

## Experimental Vulkan backend

The in-development Vulkan backend uses driver-advertised float32/float64
arithmetic and checked native conversions. Its explicit fallback bundle retains
native binary32 while replacing unsupported double arithmetic/casts with integer
GPU code. ReLU, transport, identity casts and bit-preserving operations do not
require double arithmetic. Missing/corrupt artifacts and device faults are not
fallback triggers. This backend is still being qualified for released CLI and
relocatable packages; see the [Vulkan implementation boundary](../native-vulkan-roadmap.md).
