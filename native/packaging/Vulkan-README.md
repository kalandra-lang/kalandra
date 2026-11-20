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
kernels without float64 division still use hardware. Fallback replaces float64
arithmetic for the selected kernel as a whole while preserving native float32. NVIDIA RTX 5090 passes the native
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

### Leak checking on NVIDIA Linux

On NVIDIA 620.70.06, the desktop Vulkan driver leaks host allocations when it
probes EGL devices and D-Bus. For a foreground compute program, generate an
NVIDIA EGL driver manifest in your application directory using Python 3:

```sh
kalandra-vulkan-nvidia-icd > program/nvidia-compute.json
VK_DRIVER_FILES="$PWD/program/nvidia-compute.json" __GL_DisableDBus=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./program/program
```

This selects NVIDIA for this invocation and skips its desktop D-Bus probe.
Other processes retain their driver settings. Pass the installed NVIDIA ICD
JSON path to the helper if your distribution uses a different location.
Regenerate the file after a driver update. Intel Vulkan passed with its stock ICD.

Pinned MIT ggml sources retain their notices under `share/licenses/kalandra-vulkan`.
The configured SPIR-V validator's copyright notice and binary digest are retained.

On Windows, run from the matching Visual Studio developer command prompt with
C++ Build Tools and the Windows SDK available. The generated executable is
`program\program.exe`; keep its SPIR-V files beside it. Native x86-64 and ARM64
compiler/package CI does not establish physical Windows GPU correctness or
performance. Those hardware tests are deferred; a compatible vendor Vulkan
driver is required. macOS continues to use Metal.
