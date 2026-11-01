# Kalandra native CUDA compiler (Linux, experimental)

Extract the complete archive and place its `bin` directory on PATH. Keep `lib`,
`libexec` and `share` beside it. The standalone archive includes the compiler's
private LLVM tools and host linker; it still needs the system C++ development
headers/runtime and an NVIDIA driver supporting PTX 9.2. It includes no CUDA SDK,
NVRTC, cuBLAS or driver libraries. A CUDA toolkit is needed to build this package,
but not to compile or run programs from an installed package.

Compile with `kalandra --target cuda -O2 program.k -o output`, then run
`output/program`. Deploy the entire output directory, including PTX companions
and third-party notices. Missing package resources fail closed.

Arithmetic defaults to hardware. `--arithmetic software` explicitly selects
software floating arithmetic; `--arithmetic hardware-with-software-fallback`
allows a floating capability fallback. Matrix products default to `fixed-tree`.
`--matmul-policy optimized` permits full-float32 FMA and a different accumulation
order. Eligible products use the audited MIT SGEMM_CUDA kernel; unsupported
shapes use a native-arithmetic fixed-tree GPU companion. No CPU or cuBLAS
substitution occurs. The optimized policy does not use TF32 or reduced precision.

macOS continues to use native Metal. This package does not add a CUDA cross-GPU
target, a sanitizer CLI profile, or claim qualification on GPUs other than those
listed in the CUDA roadmap evidence.
