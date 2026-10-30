# Pinned MLX Steel subset

Source: https://github.com/ml-explore/mlx at commit
`1ba7f969ad05d86decfeeb83787cc9b79e440962`.

Exactly fifteen transitive headers needed by the ordinary SIMD-group GEMM path
are embedded in `SteelUpstream.h`. Each is covered by the pinned repository's
MIT license. `UPSTREAM.json` records every original and flattened-body SHA-256.
Original source bodies and copyright notices are unchanged. Quoted includes are
flattened and `#pragma once` directives removed so the compiler can embed Metal
source without locating an external MLX installation. The full MIT notice is
included in the generated shader, bundle notices and installed Metal package.

This subset does not include NAX kernels, the MLX framework runtime, MPSGraph,
Accelerate, CUTLASS, the CUDA DSL or any separately licensed dependency. The shared
headers declare additional types, but the adapter instantiates only float32
operands and float32 accumulators. It does not convert inputs to reduced precision.

Run `python3 vendor.py` for the offline integrity/closure/license audit. With
`--upstream /path/to/pinned/mlx`, it also verifies every original header against
that exact Git revision and confirms the embedded source bodies are unchanged.
The audit reconstructs the complete embedding and both license notices, rejects
extra source outside the named bodies, and checks the original quoted dependency
closure when the upstream checkout is supplied.

The owned adapter in `native/metal-source/SteelMatmul.h` supplies entry bindings,
the existing shape ABI, profile selection and Kalandra's validated fused post-map.
It calls upstream loaders, `GEMMKernel::gemm_loop`, SIMD accumulation and stores.
No upstream kernel arithmetic is rewritten. Qualification and benchmarking of
the adapter remain Kalandra's responsibility.
