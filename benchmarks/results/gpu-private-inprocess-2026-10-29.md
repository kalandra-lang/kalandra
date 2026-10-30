# Same-runtime GPU allocation investigation — 29 October 2026

This diagnostic keeps one runtime, queue, prepared program and resident inputs alive across paired ABBA/BAAB intervals. Production remains unchanged. Cache switching occurs only at quiescent boundaries; idle storage is trimmed and two complete 1,024-call warm-up intervals precede every measured interval. Both sides enable GPU/encoding counters. Each case uses five blocks and two initial warm-up slots. Setup, trimming and warm-up are outside measured intervals; all per-invocation work is inside. These instrumented observations are not uninstrumented production acceptance.

576 full-output comparisons pass across eight shapes, both switching orders, all runner policies/upload modes, with opposite-pattern warm buffers before each observed result. Sixteen independent high-precision oracle checks pass; the float32 decoder preserves signed zero. This checks numerical behavior of the diagnostic, not full production ownership/fault qualification.

The precision goal declared before the first final AA output was absolute median wall-time drift ≤2% and every block within ±5%. A failed control cannot establish a roughly 10% total-time benefit. No samples are removed. GPU timestamps measure complete command intervals, including stalls or scheduling, not exclusive arithmetic time. Encoding scopes are inclusive and must not be added to GPU time. Wall minus summed GPU duration is elapsed time outside those timestamp intervals, not CPU consumption.

| Environment stage | Study | Rows / calls per slot | Wall reduction | Wall block range | GPU interval reduction | Encoding reduction | AA precision goal |
|---|---|---|---:|---:|---:|---:|---|
| Before cleanup | aa | 64 / 1024 | +10.77% | -19.39% … +25.17% | +17.57% | +1.34% | fail |
| Before cleanup | toggle | 64 / 1024 | +7.01% | +4.66% … +22.60% | -3.79% | +39.23% | — |
| Before cleanup | repeat | 64 / 1024 | +17.86% | -14.87% … +22.18% | +14.39% | +39.74% | — |
| After graceful signal (partial cleanup) | aa | 64 / 1024 | -9.77% | -24.67% … +23.77% | -18.68% | +0.54% | fail |
| After graceful signal (partial cleanup) | toggle | 64 / 1024 | +3.43% | -24.19% … +9.17% | -6.04% | +36.96% | — |
| After graceful signal (partial cleanup) | repeat | 64 / 1024 | -2.10% | -16.87% … +23.14% | -14.07% | +38.53% | — |
| After verified replay termination | aa | 64 / 1024 | +2.67% | -4.22% … +30.54% | +6.69% | +0.91% | fail |
| After verified replay termination | toggle | 64 / 1024 | +22.79% | -32.40% … +28.05% | +21.62% | +38.49% | — |
| After verified replay termination | repeat | 64 / 1024 | +20.60% | -17.76% … +30.96% | +20.50% | +40.27% | — |
| Mislaunched64-row shorter intervals | aa | 64 / 256 | +17.02% | -55.18% … +36.63% | +21.16% | +2.18% | fail |
| Mislaunched64-row shorter intervals | toggle | 64 / 256 | +9.15% | -34.78% … +28.71% | -1.87% | +33.12% | — |
| Mislaunched64-row shorter intervals | repeat | 64 / 256 | +18.34% | -29.86% … +38.38% | +13.64% | +39.89% | — |
| Validated512-row larger workload | aa | 512 / 256 | +1.72% | +1.45% … +7.71% | +2.29% | +3.66% | fail |
| Validated512-row larger workload | toggle | 512 / 256 | +15.21% | +10.90% … +20.34% | +7.30% | +35.94% | — |
| Validated512-row larger workload | repeat | 512 / 256 | +13.39% | +10.29% … +18.00% | +5.28% | +33.96% | — |
| Longer512-row intervals | aa | 512 / 1024 | -0.22% | -2.88% … +2.49% | +0.01% | +0.28% | pass |
| Longer512-row intervals | toggle | 512 / 1024 | +13.01% | +11.56% … +17.31% | +5.31% | +37.48% | — |
| Longer512-row intervals | repeat | 512 / 1024 | +14.18% | +13.71% … +14.78% | +5.52% | +39.06% | — |

## Environment investigation

The first identical-build control already fails in a single long-lived process. Its GPU command intervals vary widely while encoding is much steadier, so independent-process startup is not a sufficient explanation. Read-only GPU telemetry after that series reports no recorded GPU recovery; a post-run idle utilization snapshot cannot establish idleness during measurements.

Sixteen live replay services were identified from open trace paths belonging exclusively to this task. Graceful SIGTERM did not stop all of them. The clean-named files therefore describe partial cleanup; they are not evidence that every service had exited. A later verified-termination cohort retains the same frozen executable and methodology. Saved cleanup records distinguish requested signals, trace ownership and verified exits. Xcode, Instruments and unrelated user services are outside that process-cleanup scope. Ordered before/after cohorts cannot alone prove that replay services caused every timing change.

## Reproduction

`private_output_inprocess.py --phase prepare --build NEW --output NEW/prepare.json`, followed by `--phase verify --output NEW/qualification.json`, builds and qualifies a copied runtime. For timing use `--phase measure --study aa|toggle|repeat --scope largest --count 1024 --blocks 5 --build NEW --output FRESH.json`. Redirect full output to files; record external load. Never overlap timing with builds, tests or profiling. Frozen artifacts stay local under ignored `benchmarks/.build`; raw observations, hashes, numerical qualification and environment records remain retained. Run this auditor to recompute all paired metrics before interpretation.

## Frequency and workload follow-up

A separate diagnostic samples rootless macmon reported DVFS residency while timing the unchanged runtime. Twenty intervals have >99% telemetry coverage. Reported weighted frequency spans about 1,014–1,397 MHz; GPU interval time correlates with inverse frequency (Pearson 0.919). This is a strong association, not proof of sole cause or an instantaneous hardware clock. Sampling deliberately overlaps this profiling cohort only, never acceptance. The temporary MIT helper is isolated from production; its upstream revision and source/license hashes are recorded.

The first large-named cohort was accidentally launched with the 64-row base tool. Its metadata records the true shape; those samples remain retained and are excluded from larger-workload conclusions. The corrected 512×512 workload adds 72 full-output comparisons and two oracles, keeping shaders/runtime unchanged. With 1,024 calls per slot the identical-build control finally meets the original precision goal: -0.22% median, every block within ±2.89%. Enabled reuse reduces elapsed time 13.01%, independently repeated at 14.18%; every enabled block improves. These are prototype results. Production promotion requires its own ownership/fault/cancellation qualification and fresh measurements.

Replay cleanup did not make the 64-row control pass, so no small-workload total-time claim follows. Every unfavorable and mislaunched cohort remains in the table.
