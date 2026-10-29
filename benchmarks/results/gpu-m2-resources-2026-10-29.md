# M2 shader resources: measured registers, spills and occupancy

Xcode 27.1's **GUI GPU replay profiler works on this Apple M2 Max**. The newer
`gpudebug profile run` limitation did not justify leaving this investigation at
hypotheses. No production code or arithmetic policy changes in this study.

Scope: the strict float32 FFN contraction, M×K×N = **64×2048×512**. Both shaders
come from the same frozen, previously numerically qualified library. The baseline
uses the existing register16 tile; the rejected quad computes four outputs per
thread. Capturing the quad repeats exact GPU/CPU-tree, input-immutability and guard
checks, all passing. The Instruments baseline driver completes all 22 batches,
including two warmups, with those checks. No new full native suite was needed for
this diagnostic-only change.

## Direct Xcode observations

The allocated-register/high-register/spill fields are from the Shaders table;
temporary registers and maximum theoretical occupancy are from the Pipeline State
sidebar. They are **distinct displayed fields**, not interchangeable counts.
The compiler fields were transcribed from the GUI, not inferred from source.

| Field | Existing register16 | Rejected four-output tile |
|---|---:|---:|
| Allocated registers | 100 | 254 |
| High register | 100 | 254 |
| Spilled bytes | 0 | 592 |
| Temporary registers | 52 | 128 |
| Maximum theoretical occupancy | 29.167% | 12.500% |
| Measured kernel occupancy, encoder export | 24.81% | 6.79% |
| Memory-wait instruction cost | 51.003% | 63.463% |

The original shader does **not** spill in this capture. The quad exhausts more
register resources, spills, and supports much less concurrent work. This gives
concrete evidence behind its rejection. It does not prove that registers are its
only changed bottleneck: tile size, threadgroup storage and dispatch geometry also
change.

The exported encoder counters show why counting explicit shared-memory loads was
misleading:

| Xcode counter (as reported) | Existing register16 | Rejected quad |
|---|---:|---:|
| Threadgroup memory bytes read | 150,994,944 | 75,497,472 |
| Buffer L1 bytes read | 8,407,424 | 38,631,424 |
| Buffer L1 bytes written | 32,768 | 33,685,504 |
| Bytes read from device memory | 4,726,656 | 4,856,128 |

The quad halves reported threadgroup reads but greatly increases buffer-cache
traffic, consistent with its reported spilling. This is not a measurement of
application-level byte counts or a claim that all extra traffic reaches DRAM.

For the original shader, threadgroup-read limiter is **32.90%**, ALU limiter
**25.61%**, and buffer-read limiter **3.30%** in the encoder export. Memory-wait
instruction cost is a sampled shader cost category, **not 51% of application wall
time**, and does not by itself identify off-chip bandwidth as the problem.

## Independent Instruments cross-check

A sequential Metal System Trace plus Metal GPU Counters recording captures
2,225,242 device-counter rows. Its GPU command intervals match all 22 driver GPU
durations to within 100 ns. Analysis excludes two warmup batches, trims 100 µs
from each measured batch edge, and separately excludes other processes' GPU
activity with a 20 µs margin. Device counters are global; this exclusion is a
control for attribution, not a per-process hardware counter API.

The following are sample means in the remaining target-only intervals, each
using 57,358 samples across the 20 measured batches:

| Counter | Sample mean |
|---|---:|
| ALU Limiter | 36.82% |
| ALU Utilization | 33.63% |
| F32 Utilization | 18.82% |
| Buffer Read Limiter | 4.67% |
| Threadgroup/Imageblock Load Limiter | 43.38% |
| Compute Occupancy | 24.58% |

Occupancy agrees closely with the separate Xcode replay. Threadgroup loads again
exceed the ALU and device-buffer read limiters. Absolute limiter values differ
between sustained execution and instrumented replay; these are separate profiling
regimes, not interchangeable performance samples. Some Instruments bandwidth,
cache and MMU counters remain zero; **do not interpret them as zero traffic**.
Xcode's encoder export provides nonzero values for those subsystems.

## What to optimize next

The current kernel has limited theoretical occupancy, no spills, and significant
shared-memory waiting. The next experiment should reduce live temporary values
and/or threadgroup-load pressure, with one change at a time and the same reduction
tree. Re-measure register allocation, spills and occupancy before accepting a
layout. A wider per-thread output tile is the wrong starting point given this
measured resource cost. New timing acceptance must still use unprofiled frozen
pairs, exact numerical checks and whole-model/resource/cancellation gates.

This finding concerns one FFN contraction shader. It does not explain every
kernel or the separate concurrent-encoder whole-model regression. No performance
gain is claimed here. Xcode full profiling changes execution conditions (the quad
summary reports Medium performance state); its replay durations are not benchmark
results, and no ratio of those durations is used as acceptance.

## Reproduction

1. Use the existing capture driver and pinned `quad/study.metallib`. Capture one
   baseline or current dispatch with `MTL_CAPTURE_ENABLED=1` and an absolute
   `KALANDRA_PROBE_CAPTURE` destination; arguments are
   `LIBRARY baseline|current 64 2048 512 1 1 0`.
2. Open each `.gputrace` in Xcode. Enable **Profile after replay**, then Replay.
   Performance → Shaders exposes allocated registers and spills. Expand the
   encoder, select the shader, and show the Pipeline State sidebar for compiler
   statistics and runtime instruction costs. Performance → Share → Export Encoder
   Counters produces the raw CSV. Stop replay before the next profiling run.
3. For sustained counters, run `xcrun xctrace record --template 'Metal System Trace'
   --instrument 'Metal GPU Counters' --output TRACE --time-limit 20s
   --target-stdout LOG --launch -- DRIVER LIBRARY baseline 64 2048 512 256 20 0`.
   Export the TOC, `gpu-counter-info`, `gpu-counter-value` and
   `metal-gpu-intervals` tables. The counter-value schema is supplied by the
   installed GPUBase Instruments package; its last columns are sample and ring
   indices, not profile identifiers.
4. `native/benchmarks/fair/m2_counter_report.py` validates matching driver/trace
   durations, resolves XML references and writes bounded summaries from the dated
   ignored artifacts. It keeps every selected counter sample. The public Metal
   counter inventory exposes timestamps only and is recorded separately.

The initial Instruments GUI Summary showed no counter detail even though its raw
counter table contained data. Exporting metadata alone was insufficient; the
2.2 million actual sample rows were decoded and tied to target intervals. A first
exploratory parser assumed every exported table had a schema element and stopped;
the retained XML was subsequently processed with reference-aware streaming code.
Raw traces/XML remain ignored (the combined XML is about 313 MiB). Tracked files
contain small exports, summaries and source/artifact hashes.

[Resource observations and audit](gpu-m2-resources-2026-10-29.json),
[baseline raw counters](gpu-m2-baseline-counters-2026-10-29.csv),
[quad raw counters](gpu-m2-quad-counters-2026-10-29.csv).
Apple's [occupancy documentation](https://developer.apple.com/documentation/xcode/finding-your-metal-apps-gpu-occupancy)
and [Performance timeline guide](https://developer.apple.com/documentation/xcode/analyzing-apple-gpu-performance-using-a-visual-timeline)
explain the resource ceiling and compiler/runtime distinction used here.
