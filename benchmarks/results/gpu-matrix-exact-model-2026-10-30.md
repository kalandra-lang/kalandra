# Exact-oracle matrix correctness — 30 October 2026

29 cases × three implementations × three executions: **350,208 output cells** checked, with Metal API/shader validation, unchanged inputs and input/output guards. These are correctness checks on frozen benchmark-only kernels, not performance acceptance or production promotion.

## What was independently checked

The oracle decodes each finite float32 into an exact integer times 2^-149, forms exact integer products and sums at scale 2^-298, and checks the finite error inequality using integers. No floating-point summation is used as ground truth. The decoder/rounder also has independent Decimal identity checks and explicit ties-to-even witnesses.

A second integer model implements nearest-even float32 operations with hardware flush-to-zero: (a) separately rounded products and the existing adjacent-pair tree; (b) sequential fused multiply-add from +0. **Every tested current-kernel output matches model (a), and every tested SIMD-prototype output matches model (b)**, allowing arbitrary NaN payloads. This is a measured model match on this device and corpus, not a promise that every future Metal implementation uses that accumulation order.

Coverage includes three independently randomized 64×576×64 problems, contraction lengths 512–4096, identity selection, ones, mantissa-sensitive inputs, cancellation, signed zeros, minimum normal/maximum finite values, subnormal inputs/results, infinities, quiet/signaling NaNs and overflow.

## Why passing an error bound is not universal accuracy

| Case | Exact real result, first cell | Current tree | SIMD prototype |
|---|---:|---:|---:|
| Repeated `[1e20, 1, -1e20, 1]`, 512 terms | 256 | 0 | 1 |
| Repeated `[-4096×4098, 4097×4097]`, 512 terms | 256 | 0 | 1 |
| Small residual `[1, 2^-20, -1, 2^-20]`, 512 terms | 0.000244140625 | exact | exact |
| Intermediate product overflow followed by cancellation | 0 | NaN | +infinity |

The first two examples pass the conservative sum-of-absolute-products bound despite approximately 100% relative error. Severe cancellation makes that bound large relative to the answer; this affects both implementations. “Strict” means a specified calculation order, not exact real arithmetic or guaranteed greater accuracy. The overflow example also demonstrates a categorical difference, beyond a few last bits.

On the three randomized cases, maximum per-cell relative errors for current/SIMD are:

| Seed | Current | SIMD |
|---|---:|---:|
| 0 | 0.00044303024 | 0.0025766097 |
| 1 | 6.6416768e-05 | 0.00015338236 |
| 2 | 4.5660056e-05 | 0.00012613741 |

These maxima concern this test distribution, not general error guarantees. No evidence here supports automatically discarding input precision. The mantissa-sensitive witness is preserved, but it does not prove precision for every possible input.

## Exceptional values and scope

Both paths produce the expected result classes for the isolated infinity, NaN, zero-times-infinity and finite-overflow cases. Tested subnormal inputs/products flush to zero in both paths, consistent with the existing hardware policy; they do not meet a gradual-underflow real-number bound. Negative-zero sums stay negative in the current tree and become positive in the prototype. NaN payload equality and floating-point exception flags are not promised or tested.

The primitive is verified for the specified fused arithmetic on the tested aligned domain. It **does not implement current matmul semantics**, and is not universally more accurate. Production remains unchanged. A visible arithmetic-policy decision, shape-tail/fallback qualification, whole-model checks and cancellation/resource integration are still required before promotion.

The initial exact-bound-only run is also retained. Its original source is reconstructible from `matrix-exact-pre-model-2026-10-30.patch` applied to the final verifier; the second run adds the two independent operation models without changing the GPU kernels. An initial sandboxed invocation could not access Metal; rerunning the same binary with GPU access passed.
