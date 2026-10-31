# One-billion-iteration CPU benchmark — local experiment

Kalandra is effectively tied with C++ and Rust on this workload. Its sub-percent primary median lead is not a demonstrated general speed advantage. Nothing was pushed to GitHub.

## Environment and method

- Host: Apple M2 Max; macOS-27.2-arm64-arm-64bit-Mach-O.
- Candidate: kalandra 0.99.1 + local experimental APIs; local API source commit `94bf569c57f7479cd25da0b02fde4a90b05b860e`; `-O2`.
- C++: Homebrew clang version 23.1.3; C++23, `-O3`, macOS minimum 26.
- Rust: 1.93.0, LLVM 21.1.8; `-C opt-level=3`, macOS minimum 26.
- Main case: runtime `u=40`, 10,000 rows × 100,000 inner iterations = 1,000,000,000 remainder/add pairs. Sequential CPU; no GPU, parallel mapping or numerical library.
- End-to-end process time includes argument parsing, bounded randomness, allocation, computation and output. Each output encodes the actual random value and selected result for independent validation.
- Seven shuffled complete blocks, one full warmup per executable, unchanged binary hashes, and an identical C++ executable repeated as a control. No builds, tests, profiling or plotting overlapped timing.

## Primary results

| Build | Median | Observed range |
| --- | ---: | ---: |
| Kalandra prototype | 558.70 ms | 557.08–571.90 ms |
| C++ optimized | 561.11 ms | 559.74–683.43 ms |
| Rust optimized | 562.52 ms | 560.01–623.03 ms |
| C++ checked accumulation | 568.69 ms | 557.20–611.77 ms |
| Rust checked overflow | 617.85 ms | 592.30–631.34 ms |

The identical C++ control median differed by 0.23%. Kalandra keeps checked arithmetic; ordinary C++/Rust builds do not enable overflow checks. Both checked controls are shown separately. The compared inputs never overflow int32.

## Independent confirmation

| Build | Fresh-order median (3 blocks) |
| --- | ---: |
| kalandra | 556.84 ms |
| cpp | 563.06 ms |
| rust | 566.46 ms |
| cpp-repeat | 561.05 ms |

The repeated C++ binary differed by 0.36% in this confirmation. Both studies support near-parity, not a strong sub-percent ranking.

## Equivalent-work and correctness evidence

- All five unique executables passed eight input combinations. Every array element was checked against an independent integer arithmetic-series formula: 40 full-array cases and 50,565 elements total, including all 10,000 elements of the billion-iteration case for each executable.
- Every warmup, scaling trial, timed sample and confirmation output passed checks using its actual reported random value. Frozen executable hashes matched before/after both studies.
- Final optimized disassembly retains the nested repeated work. C++ and Rust unroll their inner loops; Kalandra lowers tail recursion to a loop. No mathematical shortcut or precomputed row is used.
- Kalandra uses checked int32 accumulation; LLVM widens its loop induction/remainder to signed 64-bit division. The references use 32-bit division. These are compiler code-generation differences, not different source numeric types or a handwritten kernel.
- Reference arrays retain the original zero initialization. Kalandra generate initializes output cells once instead; all three perform the same billion remainder/add pairs. This small initialization difference is included rather than hidden.
- Random generators differ (Kalandra uses a locked, entropy-seeded MT19937; references use the macOS system bounded RNG). Each program draws once outside the arithmetic loops.

| Rows (100,000 inner iterations) | Kalandra | C++ | Rust |
| ---: | ---: | ---: | ---: |
| 2,500 | 141.90 ms | 143.14 ms | 144.32 ms |
| 5,000 | 283.65 ms | 283.89 ms | 289.19 ms |
| 10,000 | 560.46 ms | 560.23 ms | 565.63 ms |

The near-linear row scaling supports the assembly audit: the inner work is repeated for each row.

## Load and limitations

macOS media analysis and Spotlight maintenance remained active after the user confirmed application workloads were idle. These are not uncontended absolute-performance claims. Background CPU snapshots and every unfavorable elapsed sample are retained.
A pre-run load check paused the primary study at 411.7% aggregate CPU before the next executable launched. The collector then waited at the same 400% threshold, resumed the original order with unchanged executables, and retained all 34 already completed timed samples. No executed timing was discarded. The independent confirmation used the same threshold and binaries.
Only this integer workload and this machine/toolchain combination are measured. Do not compare these timings directly with the source website’s different M3 Mac or infer general language/GPU performance.

## Reproduction and raw evidence

Commands, API contracts and prototype limits: [README](README.md). Sources: [Kalandra](benchmark.k), [C++](reference.cpp), [Rust](reference.rs), [API tests](test_apis.py), [runner](run.py). Original benchmark: https://benjdd.com/languages2/.

Ignored local files under `build/billion-loops/`:

- `results.json`: primary observations, binary/source hashes, load snapshots, pause record and summary.
- `replication.json`: fresh-order confirmation and unchanged binary hashes.
- `correctness.json`: every full-array reference check.
- `interrupted-before-resume.json`: preserved primary partial study before continuation.
- `*.ll`, `*.asm`: optimized IR and executable disassembly.

API checks passed at O0/O2 and with ASan/UBSan, including invalid inputs, integer boundaries, empty/oversized generation, callback failures, sequential indices and distinct RNG calls. Worker use was additionally checked with TSan. Fourteen existing array, closure, ordinary-entry and call-foundation regressions passed.
