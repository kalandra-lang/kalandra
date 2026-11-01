# Kalandra CPU/Vulkan compiler

This package retains CPU compilation and adds `--target vulkan` for GPU programs.
GPU compilation uses the adjacent private LLVM/SPIR-V tools and runtime archives.
It requires system C++23 development headers and the platform C++ runtime. GPU
execution requires the system Vulkan loader and a supported physical GPU driver;
no driver or system SDK library is redistributed in the package.

```sh
kalandra --target vulkan program.k -O2 -o program
./program/program
```

The output directory contains the executable and generated SPIR-V. Keep them
together when copying the application. Project/package options are shared with
the CPU compiler. Native hardware arithmetic is the default. Use
`--arithmetic software` for the explicit integer float64 implementation, or
`--arithmetic hardware-with-software-fallback` to bundle an alternative selected
only for unsupported native capabilities. A missing or malformed primary module
is an error, not a software-fallback trigger.

Native float64 requests supported preservation controls. Intel float64 division
currently reports Unsupported because the tested hardware division does not
satisfy the edge-case numerical contract. Select explicit
`--arithmetic hardware-with-software-fallback` for that operation on Intel;
add/subtract/multiply still use hardware. NVIDIA RTX 5090 passes the native
four-operation edge corpus. Hardware-only mode never selects software silently.

Matrix products default to `--matmul-policy fixed-tree`. Explicit `optimized`
selects the imported float32 tiled algorithm, permitting fused multiply-add and
a different reduction order with full float32 operands/accumulators. Its private
GPU right-operand transpose consumes an additional buffer and dispatch. Both
remain inside the normal allocation/cancellation contract. Software arithmetic
retains fixed-tree products. Unsupported tile limits select the adjacent native
fixed-tree companion.

Linux packages provide `--sanitize` for generated host/runtime code and CPU
exports; it does not instrument GPU instructions. Public GPU `--thread-sanitize`
and cross-target GPU builds are currently unsupported. Transpose supports all
ten storage types. GPU softmaxRows/rmsNormRows require native float32, matching
the existing Metal source contract.

Pinned MIT ggml sources retain their notices under `share/licenses/kalandra-vulkan`.
The configured SPIR-V validator's copyright notice and binary digest are retained.
