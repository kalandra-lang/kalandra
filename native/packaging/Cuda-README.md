# Kalandra native CUDA compiler (Linux and Windows, experimental)

Extract the complete archive and place its `bin` directory on PATH. Keep `lib`,
`libexec` and `share` beside it. The standalone archive includes the compiler's
private LLVM tools and host linker; it still needs the system C++ development
headers/runtime and an NVIDIA driver supporting PTX 9.2. It includes no CUDA SDK,
NVRTC, cuBLAS or driver libraries. A CUDA toolkit is needed to build this package,
but not to compile or run programs from an installed package.

Compile with `kalandra --target cuda -O2 program.k -o output`, then run
`output/program` (`output\program.exe` on Windows). Deploy the entire output directory, including PTX companions
and third-party notices. Missing package resources fail closed.

Arithmetic defaults to hardware. `--arithmetic software` explicitly selects
software floating arithmetic; `--arithmetic hardware-with-software-fallback`
allows a floating capability fallback. Matrix products default to `fixed-tree`.
`--matmul-policy optimized` permits full-float32 FMA and a different accumulation
order. Eligible aligned products use pinned BSD-3-Clause CUTLASS C++ SIMT
kernels, with five independent contraction partitions and a deterministic
reduction. Narrow/wide output tiles are selected from shape alone. Temporary
workspace is budgeted and can be reused in bounded row bands. If it cannot be reserved, a scratch-free GPU kernel
computes the same arithmetic order. Unsupported shapes use a native-arithmetic
fixed-tree GPU companion. No CPU or cuBLAS
substitution occurs. The optimized policy does not use TF32 or reduced precision.

macOS continues to use native Metal. This package does not add a CUDA cross-GPU
target, a sanitizer CLI profile, or claim qualification on GPUs other than those
listed in the CUDA roadmap evidence.

Windows builds require the matching Visual Studio C++ Build Tools and Windows
SDK, available in a developer command prompt. Compiler packages are built for
x86-64 and ARM64. Hosted Windows CI checks compiler/package behavior, native CPU
execution and GPU source compilation. Physical Windows GPU execution and
performance are not yet qualified. An ARM64 compiler build does not imply that
NVIDIA supplies a compatible ARM64 CUDA driver. macOS uses Metal.
