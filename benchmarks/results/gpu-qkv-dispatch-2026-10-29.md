# Strict Q/K/V dispatch screen

Apple M2 Max, 29 October 2026. Isolated kernel evidence only; production is unchanged. This experiment compares a two-dimensional batched dispatch with three ordinary dispatches in a concurrent encoder. Each thread retains the original register16 products and adjacent-pair reduction tree. There is no FMA, reassociation, input packing, or vendor numerical library.

## Controls and paired outcomes

The split control uses three encoders and three dispatches. The serial control uses one encoder and three unchanged dispatches. The dispatch candidate uses one encoder and one dispatch, with separate input-weight and output buffers for Q, K and V. The concurrent candidate uses three unchanged register16 dispatches in one concurrent encoder. Both controls use the same frozen executable and shader library as the candidate. Positive percentages mean less elapsed time; percentages are medians of paired ratios, not ratios of the displayed medians. No sample or outlier is discarded.

| Candidate / control | M×K×N (each projection) | Control µs | Candidate µs | Time reduction | Winning blocks | GPU time reduction |
|---|---|---:|---:|---:|---:|---:|
| dispatch / split | 16×512×512 | 63.497 | 28.022 | +57.14% | 7/7 | +58.10% |
| dispatch / split | 16×1024×1024 | 151.945 | 105.416 | +30.55% | 7/7 | +30.31% |
| dispatch / split | 64×512×512 | 88.899 | 79.622 | +10.29% | 7/7 | +9.40% |
| dispatch / split | 64×1024×1024 | 340.385 | 335.654 | +1.25% | 6/7 | +1.22% |
| dispatch / serial | 16×512×512 | 63.041 | 27.365 | +56.59% | 7/7 | +57.54% |
| dispatch / serial | 16×1024×1024 | 152.369 | 106.476 | +30.17% | 7/7 | +30.36% |
| dispatch / serial | 64×512×512 | 88.102 | 79.512 | +9.75% | 7/7 | +9.63% |
| dispatch / serial | 64×1024×1024 | 340.522 | 336.890 | +0.74% | 7/7 | +0.75% |
| concurrent / serial | 16×512×512 | 63.516 | 31.811 | +49.36% | 7/7 | +50.93% |
| concurrent / serial | 16×1024×1024 | 151.047 | 105.788 | +29.96% | 7/7 | +30.39% |
| concurrent / serial | 64×512×512 | 87.288 | 80.006 | +8.01% | 7/7 | +8.48% |
| concurrent / serial | 64×1024×1024 | 339.860 | 306.070 | +9.81% | 7/7 | +9.92% |

## Qualification and timing scope

162 validation case executions passed: two frozen candidates × nine shapes × three input patterns × three execution modes. Every output bit is compared with the original GPU entry. Finite mixed-scale and signed-zero patterns also use a separately rounded CPU adjacent-pair oracle; the IEEE pattern checks compatibility with existing GPU behavior, not independent IEEE conformance. Weights differ between the three projections to expose incorrect buffer selection. Every input and every output guard is checked. Odd tails and non-power-of-two contraction lengths are included. Metal API and shader validation were enabled during qualification.

Seven ABBA/BAAB blocks per shape use fresh processes, two warmups and three measured batches. Batches target 30 ms, capped at 256 three-projection invocations; both sides use the same count calibrated from the faster side. Every warmup and measured batch checks all output bits, input immutability and guards outside timing. Timings include command encoding/submission/wait with preallocated resident buffers. They exclude CPU oracle setup, input upload, allocation and readback. This is not whole-transformer or language-level acceptance.

Builds, qualification, profiling and screening timings were sequential. Ordinary background applications remained open. Sampled external CPU activity (100% represents one core):

- dispatch / split: 54 samples, median 163.9%, maximum 510.4%.
- dispatch / serial: 54 samples, median 111.8%, maximum 352.2%.
- concurrent / serial: 54 samples, median 209.1%, maximum 435.2%.

## Profiling limit and next gate

Before this screen, a verified baseline FFN contraction was captured with Apple’s [GPU command-line debugger](https://developer.apple.com/documentation/xcode/debugging-with-interactive-command-line-tools). Pipeline identity was confirmed, but `profile run` explicitly reported that GPU profiling requires Apple M3 / A17 Pro or later. The capture is diagnostic, not timing evidence. No register count, spill count or occupancy measurement was obtained. Instruments may expose other counters; the installed CLI recording-options listing did not expose those settings. This does not prove all M2 profiling interfaces lack occupancy support.

The subsequent [copied-runtime whole-transformer gate](gpu-independent-dispatch-2026-10-29.md) does not qualify concurrent scheduling: it retains large regressions and noisy inactive controls. The batched one-grid shader remains a separate candidate, not a production speedup. Graph matching must prove three independent compatible projections, preserve each logical result and resource accounting, retain fallback for incompatible shapes/libraries, and respect ordinary cancellation boundaries. A closed-runner-only candidate must remain conditional on its existing no-independent-progress proof. Numerical qualification here does not validate those runtime obligations.

## Evidence

- [gpu-qkv-dispatch-qualification-2026-10-29.json](gpu-qkv-dispatch-qualification-2026-10-29.json)
- [gpu-qkv-dispatch-split-2026-10-29.json](gpu-qkv-dispatch-split-2026-10-29.json)
- [gpu-qkv-dispatch-split-2026-10-29-load.json](gpu-qkv-dispatch-split-2026-10-29-load.json)
- [gpu-qkv-dispatch-serial-2026-10-29.json](gpu-qkv-dispatch-serial-2026-10-29.json)
- [gpu-qkv-dispatch-serial-2026-10-29-load.json](gpu-qkv-dispatch-serial-2026-10-29-load.json)
- [gpu-qkv-concurrent-qualification-2026-10-29.json](gpu-qkv-concurrent-qualification-2026-10-29.json)
- [gpu-qkv-concurrent-serial-2026-10-29.json](gpu-qkv-concurrent-serial-2026-10-29.json)
- [gpu-qkv-concurrent-serial-2026-10-29-load.json](gpu-qkv-concurrent-serial-2026-10-29-load.json)
- [gpu-resource-probe-2026-10-29.json](gpu-resource-probe-2026-10-29.json)
