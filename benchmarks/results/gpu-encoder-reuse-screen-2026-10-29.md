# Compute encoder reuse screen — 29 October 2026

Decision: do not promote either prototype. Production remains at `aacae818`.

The two generated runtime variants reuse a serial compute encoder inside each existing work-bounded command. One adds explicit buffer memory barriers; the other uses serial dispatch ordering. Neither changes shader arithmetic, physical dispatch count, upload policy, or submission/cancellation boundaries. Encoder and command ownership survive early failures until cleanup. This screen does not replace four-mode cancellation and failure qualification.

All 72 output comparisons passed, including regular and odd shapes, all upload modes, two input patterns, and Metal API/shader validation. Three paired ABBA/BAAB blocks per case retained all samples. Positive values below mean less time; these noisy screening results do not establish an accepted speedup.

| Shape | Upload mode | Explicit barrier | Serial default |
|---|---|---:|---:|
| 1×64 | resident | +1.47% | +1.50% |
| 1×64 | all uploads | +1.76% | +1.30% |
| 1×512 | resident | -0.05% | +1.33% |
| 1×512 | all uploads | +0.20% | -0.93% |
| 16×512 | resident | -3.90% | +1.16% |
| 16×512 | all uploads | +2.01% | -1.90% |
| 64×512 | resident | +0.68% | +0.51% |
| 64×512 | all uploads | +1.46% | -8.98% |

Most improvements were around 0–2%, with regressions and large process outliers. The explicit-barrier variant regressed the 16×512 resident case by 3.90%; serial default regressed the largest all-upload case by 8.98%. No production policy changes are justified by this screen.

Reproduce against `aacae818` with `native/benchmarks/encoder_reuse_screen.py`; frozen generated sources and executables live under `benchmarks/.build/gpu-encoder-reuse-2026-10-29/screen/`. The JSON records source/artifact digests, every invocation, every sample and all output checks.

Apple documents [default compute encoders as serial](https://developer.apple.com/documentation/metal/mtlcommandbuffer/makecomputecommandencoder%28%29?language=objc). The barrier variant explicitly orders buffer accesses; neither variant was promoted solely on numerical equality.
