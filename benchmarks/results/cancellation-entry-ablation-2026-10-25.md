# Explaining the short-chain gap — 25 October 2026

Follow-up: [the runtime entry correction](cancellation-runtime-entry-2026-10-25.md)
removes a test-hook environment scan from scalar invocation startup and measures
25 ns empty entry and a 6.9–7.6% depth-100 gap. The original ablation below is
retained as measured against the earlier runtime.

Most of the depth-100 gap is fixed outer-invocation overhead. Part is avoidable benchmark scaffolding, but removing it does not eliminate the gap: the cleaned prototype remains about **16–17% slower** than ordinary C++/Rust at 100 non-tail levels. Production compiler/runtime code is unchanged.

## Controlled ablations

Every variant uses the same generated hybrid kernel LLVM and optimized runtime. Five adjacent ABBA/BAAB blocks compare each variant with ordinary C++/Rust, at identical iteration counts. All now execute two full-batch warmups before measurement. This corrects an earlier asymmetry: the original Kalandra probe warmed individual tasks, while C++/Rust warmed full batches. With matched warmups the old heap-based probe still reproduces roughly 27%/25% overhead, so warmup mismatch was not the main cause.

| Probe variant | Zero-depth median (ns) | Depth 100 / C++ | Depth 100 / Rust | Depth 1,000 / C++ | Depth 1,000 / Rust |
| --- | ---: | ---: | ---: | ---: | ---: |
| `heap` | 173.98 | 1.277 | 1.253 | 1.065 | 1.055 |
| `stack` | 134.30 | 1.185 | 1.208 | 1.061 | 1.070 |
| `heap-no-clock` | 157.15 | 1.231 | 1.222 | 1.073 | 1.085 |
| `stack-no-clock` | 102.57 | 1.170 | 1.170 | 1.063 | 1.052 |
| `stack-clean` | 103.76 | 1.158 | 1.172 | 1.068 | 1.049 |

The C++/Rust zero-depth controls are about 1.84 ns and 3.91 ns. Zero-depth controls use seven fresh processes per version and report median per-invocation time. Paired ratios use five blocks; these are distinct diagnostics, not an additive or subtraction-based performance score. Allocation, code layout and sampling interact, so individual deltas should not be summed as exact component attribution.

- `heap`: original experiment driver, with fresh heap-allocated Control and Invocation objects and a completion timestamp per invocation. These allocations happen once per **outer invocation**, not at every recursive call.
- `stack`: identical state and lifetimes, placed in the caller frame rather than allocated on the heap. State remains alive until the worker is joined.
- `heap-no-clock`: removes the per-invocation completion timestamp used by cancellation-latency measurements. The old throughput harness incorrectly paid for it even when no cancellation was requested.
- `stack-no-clock`: combines those two changes.
- `stack-clean`: also removes duplicate stack-bound-cache setup and poisonable-limit initialization needed only by the separate guard-fusion candidate. The real runtime source boundary still caches/checks physical stack bounds; no generated stack guard is removed. This last change has little consistent effect.

## What the remaining cost represents

The cleaned zero-depth control is still about 104 ns, versus 2–4 ns for the ordinary native scalar-return controls. It initializes invocation/cancellation state, captures a cancellation return point, establishes the callback/native entry boundary, balances admission, settles the ledger, checks status/ownership and publishes completion. Some status/ledger assertions are validation overhead in this probe, so 104 ns must not be presented as an isolated immutable language cost. The benchmark intentionally includes the full experimental entry path.

At 100 levels, ordinary C++/Rust useful work takes approximately 850 ns. A fixed root cost of order 100 ns therefore consumes a substantial fraction of that budget. At 1,000 levels the cleaned prototype is about 5–7% slower, within the target. This supports a fixed-entry explanation rather than a sudden deterioration at native depth 100. The depth-100 target still fails after the cleanups; the old percentage was not purely a benchmark artifact.

Generated non-tail Kalandra frames also retain checked integer arithmetic and physical-stack bounds checks absent from these C++/Rust release kernels. The [recorded disassembly](cancellation-depth-frames-2026-10-25.txt) shows those branches. The pure hybrid recursive path has no cancellation-token poll on each call. This study does not assign an exact timing to each remaining instruction or prove that all of the residual per-frame cost is safety checking.

The next performance work is the real entry/state-lifetime path: minimize fresh outer state initialization, avoid redundant adapters and lifecycle work, and expose range facts so provably redundant arithmetic checks can be removed without changing overflow semantics. Reusing or embedding state is a prospective implementation choice, not a result counted in these benchmark scores. No cancellation, ownership, arithmetic or stack-safety behavior was disabled for these variants.

## Checks and scope

75 numerical verification cases cover all five variants at depths 0/1/100/1,000/10,000 with seeds 1/17/31. Every reported verification and timed-batch checksum is checked against the rational recurrence, including the declared repeated-sum rounding bound. Each variant passes 100 long-running tail-task cancellations with owned ledger allocations. The stack-clean variant additionally passes 100 allocation-heavy cancellations under ASan/UBSan and 100 under TSan, with empty diagnostic stderr and balanced cleanup. Clockless variants deliberately do not measure cancellation latency; their zero-valued latency fields are not speed claims.

The generated caller-stack holders live outside the function containing the cancellation landing point, so cancellation does not skip their Invocation destructors. Runtime critical-region exclusion, register restoration, callback adapters, checked arithmetic, physical stack guards and settlement checks are retained. General production scheduler integration and platform qualification remain separate.

## Evidence

- [All ablation samples, generated probe hashes, commands and cancellation checks](cancellation-entry-ablation-2026-10-25.json)
- [ASan/UBSan and TSan checks](cancellation-entry-sanitizers-2026-10-25.json)
- [Original depth study, retained without rewriting scores](cancellation-depth-scaling-2026-10-25.md)
- [Reproducible ablation runner](../../native/experiments/cancellation/entry_ablation.py)

```sh
python3 native/experiments/cancellation/entry_ablation.py /tmp/entry-ablation.json
```

Requires the artifacts from the original depth/mechanism studies; the ablation regenerates only its harness variants, preserving the original generated kernels.
