# GPU register selection and the MPSGraph gap

Against frozen `f28a71f0`, all nine affected complete-block paired medians improve; six improve in every block. Including unchanged-selection controls, 17/27 medians are positive. This change broadens selection of an existing strict float32 matrix kernel. It changes no numerical shader, CPU code, cancellation quantum or ownership path.

## Why MPSGraph is faster

Apple combines a graph compiler with tuned matrix/reduction kernels and describes stitching adjacent operations into those kernels. Kalandra currently has narrower fusion. See [Apple’s MPSGraph presentation](https://developer.apple.com/videos/play/wwdc2020/10677/?time=1049). This explains the available mechanisms, not an exact attribution of this benchmark’s gap.

Resident-input measurements exclude transfer cost as an explanation for the resident gap. Kalandra still makes 14 compute dispatches across four or seven explicit submissions, with bounded cancellation checkpoints. Our MPS harness makes one graph encode/commit API call; this does **not** establish its internal command-buffer count. The SDK says graph encoding may call `commitAndContinue`. Do not compare an MPS wrapper count with Kalandra’s actual submissions or interpret the MPS harness’s last command-buffer interval as total graph device time.

Kalandra keeps separately rounded products and an adjacent-pair reduction tree. Vendor outputs meet the independent numerical bound but need not be bit-identical. The cost of this arithmetic difference has not been isolated. Neither precision relaxation nor removal of cancellation is part of this change.

## Accepted selection rule

Register16 now admits at least 8,192 output cells instead of 65,536. Existing guards remain: both output dimensions at least 16, inner dimension 512–4096 and divisible by 64. The explicit scalar reduction roots and existing tile-tail checks are unchanged. The policy applies to ordinary matrix operations as well as prepared graphs, using dimensions rather than benchmark names. Other GPUs have not been physically qualified for this tuning.

Two isolated three-block screens are retained: admitting only long-inner rectangles, and admitting all qualified rectangles. The broader rule was selected before five-block acceptance. General matrix testing covers skinny/tall rectangles, boundary shapes, partial tiles and non-power-of-two counts of 64-leaf blocks.

## Complete-block paired acceptance

Positive percentages mean less time. Counts in parentheses are positive blocks out of five; small changes on unaffected shapes are noise controls, not demonstrated gains. All negative medians remain in the table.

| Rows × width | Resident | Activation upload | All uploads |
|---|---:|---:|---:|
| 1 × 64 | +2.77% (3/5) | -0.94% (1/5) | -0.73% (2/5) |
| 16 × 64 | -0.42% (2/5) | -1.11% (2/5) | +0.20% (3/5) |
| 64 × 64 | -0.32% (2/5) | -0.33% (2/5) | -2.64% (1/5) |
| 1 × 256 | +1.34% (4/5) | +0.72% (3/5) | +0.41% (4/5) |
| 16 × 256 | +2.75% (3/5) | +1.85% (3/5) | -0.84% (2/5) |
| 64 × 256 | +4.98% (5/5) | +3.98% (5/5) | +2.04% (4/5) |
| 1 × 512 | -2.13% (2/5) | +0.18% (3/5) | -0.01% (2/5) |
| 16 × 512 | +11.78% (5/5) | +10.38% (4/5) | +4.44% (5/5) |
| 64 × 512 | +10.40% (4/5) | +7.81% (5/5) | +5.18% (5/5) |

## General matrix acceptance

Exact dyadic integer contractions verify every output cell in both versions outside timing. Measurements include normal task creation, completion, metadata observation and retirement, with resident inputs. Positive percentages mean less time.

| M × K × N | Reduction | Positive blocks |
|---|---:|---:|
| 15 × 512 × 513 | +0.72% | 3/5 |
| 16 × 512 × 511 | +0.05% | 3/5 |
| 16 × 512 × 512 | +13.87% | 5/5 |
| 16 × 1024 × 512 | +15.04% | 5/5 |
| 16 × 2048 × 512 | +19.74% | 5/5 |
| 16 × 2112 × 512 | +21.03% | 5/5 |
| 16 × 4160 × 512 | -0.49% | 1/5 |
| 17 × 512 × 513 | +14.32% | 5/5 |
| 17 × 1536 × 513 | +20.95% | 5/5 |
| 17 × 2048 × 513 | +23.24% | 5/5 |
| 17 × 4096 × 513 | +24.14% | 5/5 |
| 32 × 512 × 256 | +13.14% | 4/5 |
| 64 × 512 × 128 | +13.66% | 5/5 |
| 64 × 1024 × 256 | +18.19% | 5/5 |
| 64 × 2048 × 512 | +21.48% | 5/5 |
| 128 × 512 × 128 | +14.30% | 5/5 |
| 256 × 512 × 32 | +13.64% | 5/5 |
| 512 × 512 × 16 | +11.63% | 5/5 |

## Fresh CPU and MPSGraph controls

Warm synchronous tensor-result API including ACK/retirement; preparation and output readback excluded for both GPUs. This is a single bias-free unmasked transformer block with 4× ReLU FFN, not model inference or token generation. Five counterbalanced control blocks are separate from the paired baseline study.

| Rows × width | CPU µs | Kalandra Metal µs | MPSGraph µs | CPU / Metal | Metal / MPS |
|---|---:|---:|---:|---:|---:|
| 1 × 64 | 14.65 | 952.95 | 300.15 | 0.02× | 3.17× |
| 16 × 64 | 67.85 | 997.81 | 303.51 | 0.07× | 3.29× |
| 64 × 64 | 263.35 | 1043.89 | 304.39 | 0.25× | 3.43× |
| 1 × 256 | 57.81 | 1011.89 | 296.82 | 0.06× | 3.41× |
| 16 × 256 | 1144.08 | 1180.44 | 368.44 | 0.97× | 3.20× |
| 64 × 256 | 2667.49 | 1442.79 | 417.44 | 1.85× | 3.46× |
| 1 × 512 | 249.87 | 966.25 | 310.37 | 0.26× | 3.11× |
| 16 × 512 | 4676.46 | 1406.30 | 445.93 | 3.33× | 3.15× |
| 64 × 512 | 10645.27 | 1943.47 | 524.40 | 5.48× | 3.71× |

## Qualification and diagnostics

- All four modes pass 1,737 typed float32 APFloat cases / 1,756,075 cell/aggregate checks per mode, including partial tiles, IEEE edges, zero reductions and 24/33-block contractions.
- Prepared graphs pass in optimized, debug, ASan/UBSan and TSan modes, including expanded fused IEEE fixtures, held cancellation/deadline/device failures, allocation failures, aliases, concurrent calls, higher ranks and long streaming fallback. No runtime lifecycle code changed.
- Fresh CPU/Metal/MPS controls verify 91 records / 476,455 cells. Frozen GPU dumps are bit-identical. Compiler binary and full-block CPU LLVM are unchanged; generated numerical shader sources are identical.
- Encoder counters are from isolated instrumented runtime copies. Sampling and clock correlation substantially perturb execution; sampled intervals are diagnostic and cannot be added to or subtracted from uninstrumented wall time. Two clock pairs and raw samples are retained; CPU timestamp units are checked empirically against Mach time rather than assumed. The counter runs verify that projections and the FFN down-projection select Register16 after the change.

- Separate runtime profile 1 × 512: wall 1.240 ms; GPU command intervals 0.445 ms; recorded encoding 0.072 ms; 4 submissions / 14 dispatches. These intervals do not form an exhaustive exclusive-time breakdown.
- Separate runtime profile 64 × 512: wall 2.037 ms; GPU command intervals 0.590 ms; recorded encoding 0.103 ms; 7 submissions / 14 dispatches. These intervals do not form an exhaustive exclusive-time breakdown.

## Remaining work

MPSGraph remains ahead, and small blocks still favor CPU. The next experiments are contraction-parallel attention-score kernels for small output matrices, additional graph fusion within the existing arithmetic and cancellation contract, and a separately controlled unified-memory transfer policy. Wider asynchronous graph scheduling needs compiler/effect proofs; the accepted change does not remove cancellation boundaries.

## Evidence and reproduction

Baseline compiler, driver, archives and generated header were frozen under `benchmarks/.build/gpu-dispatch-2026-10-28/baseline` before edits; CPU LLVM is checked against the prior progression acceptance artifact and its published hash. Rebuild `f28a71f0` to regenerate ignored baseline artifacts; use its `TransformerBlock.k` bundle and typed primitive shader. Every JSON retains commands, raw samples, source/artifact hashes and hardware/toolchain metadata. Diagnostic scripts archive that exact commit and apply instrumentation in an isolated tree. Acceptance overlaps no compilation, tests, profiling or plotting.

- [transformer-register-full-block-2026-10-28.json](transformer-register-full-block-2026-10-28.json) — SHA256 `45f094d34d05763ebcc3e9c5a45bb43b47713ce14a33d6ad4328eaeb0289f626`
- [transformer-register-paired-2026-10-28.json](transformer-register-paired-2026-10-28.json) — SHA256 `0fc33a7b32f99655943518c9bad54045e7334d2a904f4d2947125a9173c6b753`
- [transformer-register-profile-2026-10-28.json](transformer-register-profile-2026-10-28.json) — SHA256 `92a4a519698ba0319dc2067656f25afdbdf661a651b4c66cb440ccc20ebe79d2`
- [transformer-register-qualification-2026-10-28.json](transformer-register-qualification-2026-10-28.json) — SHA256 `b347d20ab717ba2ec21013dc802a358b78817f90a22aa4d9342f676b1f2b35e5`
- [transformer-register-long-screen-2026-10-28.json](transformer-register-long-screen-2026-10-28.json) — SHA256 `37905aa7d2b9992fe8350151b5bbcdb8b272c77001c1e67ea0d047b09f9a66b1`
- [transformer-register-wide-screen-2026-10-28.json](transformer-register-wide-screen-2026-10-28.json) — SHA256 `1eb4fe14b7d9be631cffd24a8cbbb90223b2a53b93944b0a195df6cc7fe29aa6`
- [gpu-register-rectangles-2026-10-28.json](gpu-register-rectangles-2026-10-28.json) — SHA256 `219e89455e38abe2a625c62624a7fa856a356e588c55cebfc3e0756843fea756`
- [transformer-dispatch-profile-2026-10-28.json](transformer-dispatch-profile-2026-10-28.json) — SHA256 `568dda7efb6ca1a57fb5f65df6f6f2ac14877f72d80c0f79e3a915adc5274693`
- [transformer-dispatch-register-profile-2026-10-28.json](transformer-dispatch-register-profile-2026-10-28.json) — SHA256 `c7bdadd6adc5d5c7a57bb84578894709a872b7fbab5b58fdd7ac010b3933d4f5`

```sh
python3 native/benchmarks/transformer_block.py --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/acceptance --output benchmarks/results/transformer-register-full-block-2026-10-28.json --blocks 5 --target-ms 30
python3 native/benchmarks/prepared_execution.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --baseline-bundle benchmarks/.build/gpu-progression-2026-10-28/acceptance/bundle --current-evidence benchmarks/results/transformer-register-full-block-2026-10-28.json --output benchmarks/results/transformer-register-paired-2026-10-28.json --blocks 5
python3 native/benchmarks/register_rectangles.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --shader benchmarks/.build/gpu-progression-2026-10-28/typed-qualification/typed/kernel0.metallib --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/rectangles --output benchmarks/results/gpu-register-rectangles-2026-10-28.json --blocks 5
python3 native/benchmarks/transformer_profile.py --evidence benchmarks/results/transformer-register-full-block-2026-10-28.json --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/profile --output benchmarks/results/transformer-register-profile-2026-10-28.json --dispatches 14 --large-submissions 7
python3 native/benchmarks/dispatch_profile.py --baseline benchmarks/.build/gpu-dispatch-2026-10-28/baseline --bundle benchmarks/.build/gpu-progression-2026-10-28/acceptance/bundle --build-dir benchmarks/.build/gpu-dispatch-2026-10-28/counters-register --output benchmarks/results/transformer-dispatch-register-profile-2026-10-28.json --register-policy wide --processes 3
```
