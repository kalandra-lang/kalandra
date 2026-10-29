# Transposed matrix kernel experiments — 29 October 2026

Decision: do not integrate these general transposed matrix profiles. The initial SIMD dot-product prototype improves small rectangles but substantially regresses large ones; shared-tile follow-ups still regress the largest cases. Production kernels remain unchanged.

These are isolated, preallocated, batched commands. Baseline executes a transpose plus the existing matrix profile; candidates read the right operand in its original row-major layout. They exclude language invocation and allocation costs and are not whole-program speedups.

The initial screen used exact dyadic inputs. The follow-up uses non-dyadic, mixed-scale values and compares every result bit with an independent CPU adjacent-pair tree, checks untouched inputs and output guard bytes, and runs all seven shapes/four profiles with Metal API and shader validation before timing. No reassociation or FMA is enabled.

Three fresh-process ABBA/BAAB blocks per case; positive values mean less whole-command time. All outliers and regressions are retained.

| M × K × N | SIMD dot, bounded roots | Transposed skinny/shared | Transposed shared tiles |
|---|---:|---:|---:|
| 1 × 64 × 1 | +75.06% | +12.21% | +9.79% |
| 1 × 512 × 1 | +66.93% | +6.07% | +6.39% |
| 3 × 37 × 5 | +63.87% | +8.67% | +14.60% |
| 16 × 512 × 16 | +6.97% | +7.73% | -164.91% |
| 17 × 33 × 9 | +67.08% | +23.45% | +21.12% |
| 64 × 512 × 64 | -191.95% | -16.05% | -51.02% |
| 64 × 2048 × 64 | -263.52% | -30.12% | -90.96% |

The largest 64×2048×64 case is 264% slower with the bounded SIMD dot profile, 30% slower with the transposed skinny/shared profile, and 91% slower with the shared-tile profile. Removing a dispatch alone does not compensate for worse execution/memory behavior.

The useful narrower observation is that transposing a singleton dimension does not change contiguous byte order. A metadata view can remove that transpose without introducing a new matrix kernel; that is being qualified separately.

Reproduction: `native/benchmarks/transpose_matmul_screen.py`, then `native/benchmarks/transpose_matmul_profiles.py`. Frozen artifacts live under `benchmarks/.build/gpu-transpose-matmul-2026-10-29/`. Raw JSON records all commands, hashes, samples and validation checks.
