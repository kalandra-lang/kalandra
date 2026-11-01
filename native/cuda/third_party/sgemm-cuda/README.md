# SGEMM_CUDA warp-tiled kernels

Original: Simon Boehm's `SGEMM_CUDA`, revision
`5a7dcc513d951ba764d51bc9d587b3163f3a894d`, MIT. The original kernel and
license are unmodified; `upstream.json` pins their SHA-256 hashes.

`adapt.py` verifies the inputs and produces two full-float32 CUDA-core tile
specializations (64×64 and 128×128, K tile 16, 128 threads). It removes unused
host/cuBLAS includes, makes the original template an inline device function,
and exports stable private entry points. The AB-only store specializes away
alpha/beta and the old C read. This avoids reading uninitialized C when beta=0.
The numerical load/warp/shared-memory algorithm is retained.

These kernels have **no tail guards**: admission must require positive M/N
multiples of the selected tile, positive K multiple of 16, non-aliasing float32
buffers, aligned float4 accesses, and checked exact extents. Other shapes need
a separately qualified fixed-tree GPU kernel. Do not round up the grid.

No TF32, FP16, Tensor Core, CuTe, CUTLASS or cuBLAS implementation is used.
FMA contraction is intentional only for explicit optimized matmul; it does
not have the fixed-tree product's rounding/order contract. NVIDIA's NVRTC
compiler is a build tool, not a production numerical library or runtime import.
The explicit optimized language policy uses the 64×64 specialization. The 128×128
specialization remains a qualification/reference candidate: two controlled
RTX 5090 series found it 18–72% slower. Ordinary matrix products retain their
fixed-tree policy; unsupported optimized shapes use a fixed-tree GPU companion.
