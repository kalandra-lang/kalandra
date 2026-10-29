# Resident host attribution and private-output feasibility — 29 October 2026

Production baseline: `dd940e07`. These tools operate on copied runtime sources; production stays unchanged. No numerical or cancellation contract change is accepted here.

## Host attribution

Instrumentation scopes are inclusive and overlap. Do not add them. Allocation counts are actual `allocateMetal` calls; destruction counts also include lightweight parameter-slice owners. Three processes, two warm-up and three measured 100-call rounds per case. Absolute times include instrumentation and prevailing host load. These are diagnostic observations, not a production speed comparison.

| Resident batch × width | Wall µs | GPU µs | Allocate µs | Destroy µs | Allocate calls | Submit→GPU µs | GPU→wake µs | Budget CAS µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 × 512 | 384.4 | 154.0 | 26.3 | 13.2 | 15 | 91.6 | 81.7 | 0.18 |
| 16 × 512 | 543.7 | 254.0 | 46.9 | 20.7 | 16 | 121.4 | 82.5 | 0.18 |
| 64 × 256 | 603.3 | 292.3 | 48.8 | 19.5 | 16 | 138.7 | 81.7 | 0.18 |
| 64 × 512 | 763.7 | 414.3 | 56.6 | 18.5 | 16 | 169.1 | 88.5 | 0.18 |

## Candidate and correctness

The probe reuses only private Float32 outputs of validated pure prepared progression. Successful eligible maps/matrix operations overwrite every physical byte. Ordinary primitive execution, transforms, packed types, empty outputs, flags, scratch and parameters retain the accepted allocation path. Last-owner retirement with zero admissions is required; no pending resource is cached. Device, exact length and shared/private kind are distinct keys. A combined 16 MiB/64-entry idle pool remains inside the existing 64 MiB payload budget. In this copied diagnostic only, counters named upload include both kinds.

288 changing-input full-output comparisons pass across both probe settings, three runner policies, three upload modes and four shapes; signed zero is preserved in decoding. Eight high-precision oracle checks pass. This does not replace production concurrency, held-ACK, failure-injection, lifetime and budget qualification.

## Timing validity

The original 250 ms identical-build control completed, with up to 44.6% absolute paired swings. The subsequent enabled screen was interrupted when the desktop app crashed. It has no complete result JSON and is not accepted evidence. Partial load data and logs are retained. The app crash report points into V8/Node buffer allocation and does not establish a causal link to GPU reuse. No surviving benchmark was found after restart.

A separately declared post-restart confirmation measures only resident64×512 with five adjacent ABBA/BAAB blocks, a 1,000 ms adaptive target, 8,192-call ceiling, two warm-up and three measured rounds per process. AA uses the identical disabled probe on both sides; production compares frozen accepted production against the enabled probe. These are separate cohorts and must not be subtracted as a causal estimate.

| Study | Batch × width / mode | Paired time reduction | Full block range |
|---|---|---:|---:|
| aa | 1 × 512 / 0 | -1.51% | -28.82% … +28.31% |
| aa | 1 × 512 / 1 | -27.58% | -29.14% … +24.58% |
| aa | 1 × 512 / 2 | +12.70% | +6.97% … +16.51% |
| aa | 16 × 512 / 0 | -27.68% | -30.01% … +19.15% |
| aa | 16 × 512 / 1 | -27.21% | -39.55% … +9.29% |
| aa | 16 × 512 / 2 | +0.23% | -1.74% … +22.99% |
| aa | 64 × 256 / 0 | -2.58% | -32.03% … +36.06% |
| aa | 64 × 256 / 1 | -14.61% | -43.17% … -11.98% |
| aa | 64 × 256 / 2 | -18.48% | -44.59% … -0.07% |
| aa | 64 × 512 / 0 | -26.19% | -37.80% … +8.00% |
| aa | 64 × 512 / 1 | -1.48% | -44.38% … +32.73% |
| aa | 64 × 512 / 2 | +16.83% | -26.01% … +28.37% |
| confirm-aa | 64 × 512 / 0 | -2.46% | -12.71% … +6.86% |
| confirm-production | 64 × 512 / 0 | +10.86% | -1.80% … +12.32% |
| broad-repeat | 1 × 512 / 0 | +10.97% | -3.31% … +12.24% |
| broad-repeat | 16 × 512 / 0 | +6.99% | +3.95% … +14.14% |
| broad-repeat | 64 × 256 / 0 | +15.31% | +9.37% … +18.07% |
| broad-repeat | 64 × 512 / 0 | -2.96% | -12.33% … +11.25% |
| broad-repeat | 64 × 512 / 2 | +1.09% | +0.42% … +4.53% |

## Interpretation

The focused +10.86% resident64×512 result does not reproduce in the broader cohort: its three blocks include both a gain and substantial slowdowns. The candidate is not promoted to production. Improvements in another size remain leads for further investigation, not a general performance claim. Broader-case differences and all negative blocks remain retained.

The additional odd/small-shape qualification passes 288 full-output comparisons and eight independent oracles on 3×33, 7×65, 1×17 and 31×31. Combined probe qualification is 576 full comparisons and 16 independent oracle checks. These do not establish lifetime/concurrency qualification or erase the failed performance replication.

Next isolate allocation/residency from driver and scheduling variance using paired intervals within one long-lived runtime, retaining identical-build controls and GPU/host timing counters. Keep production at the accepted shared-upload cache until repeated broad measurements and ownership/fault/cancellation gates justify another change.

## External load

External CPU load for aa: median 70.0%, maximum 409.7% (100% = one core). CPU sampling does not establish GPU idleness.


External CPU load for confirm-aa: median 50.8%, maximum 223.0% (100% = one core). CPU sampling does not establish GPU idleness.


External CPU load for confirm-production: median 44.5%, maximum 367.7% (100% = one core). CPU sampling does not establish GPU idleness.


External CPU load for broad-repeat: median 27.2%, maximum 283.1% (100% = one core). CPU sampling does not establish GPU idleness.


## Reproduction

`reuse_host_profile.py --frozen ACCEPTED --build NEW --output NEW.json` builds a diagnostic copy. `private_output_probe.py --phase prepare|verify --frozen ACCEPTED --build PROBE --output NEW.json` prepares and verifies the reuse probe. `private_output_confirm.py --phase measure --study aa|production --frozen ACCEPTED --build PROBE --output NEW.json` applies the focused confirmation. Never overlap measurements with compilation, tests, profiling or another benchmark. Frozen local source/artifact manifests and numerical qualification are retained; large binaries remain ignored. Run this auditor to verify hashes and paired observations before interpreting the report.
