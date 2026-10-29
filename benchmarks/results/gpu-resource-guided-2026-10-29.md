# Resource-guided strict GPU kernel experiments

**No production candidate is accepted.** Three isolated controls preserve arithmetic but fail the broad performance gate. Apple M2 Max, 29 October 2026. The existing register16 kernel remains in production.

## What changed

- **phased32:** evaluate the two 32-leaf subtrees sequentially in a non-unrolled loop. The shared tile still contains 64 leaves and retains both barriers. This differs from the previously rejected 32-leaf tile.
- **packed-b68:** transpose the right-hand shared tile and pad its column stride to 68 floats. Products and their reduction tree are unchanged. Shared storage grows from 8,192 to 8,448 bytes.
- **swizzled-b64:** transpose with a four-float XOR permutation instead of padding, preserving the original 8,192-byte allocation. This is a storage-preserving control, not a pure padding ablation: address arithmetic also changes.

All use one output per thread, the same 16×16 tile, 256 threads, 64-leaf chunks, two barriers, global matrix layout and outer reduction tree. No FMA or reassociation is introduced.

## Direct resource measurements

Xcode 27.1 full GUI replay of the qualified 64×2048×512 contraction. Register fields come from the Shaders table; temporary registers, theoretical occupancy and load instructions come from the Pipeline State sidebar. These are manually transcribed compiler fields, distinct from exported hardware counters. Replay durations are not acceptance timings.

| Variant | Allocated / high registers | Temporary registers | Spilled bytes | Theoretical occupancy | Static TG loads | Measured occupancy |
|---|---:|---:|---:|---:|---:|---:|
| baseline | 100 / 100 | 52 | 0 | 29.167% | 80 | 24.70% |
| phased32 | 108 / 108 | 56 | 0 | 29.167% | 40 | 16.72% |
| packed-b68 | 110 / 110 | 56 | 0 | 29.167% | 32 | 17.04% |
| swizzled-b64 | 111 / 111 | 56 | 0 | 29.167% | 35 | 16.93% |

The phased body executes twice per chunk: 40 static load instructions do not mean half the dynamic load work. Both transposed layouts reduce static load instructions, but all three candidate encoder exports still report 150,994,944 threadgroup bytes read, the same as the earlier baseline. Wider loads are not fewer transferred bytes. All remain spill-free; increased register counts do not establish a spilling explanation. Measured occupancy falls despite an unchanged reported ceiling. These observations do not isolate register allocation, bank behavior or instruction scheduling as the sole cause.

## All paired timings

Positive percentages mean less elapsed time; parentheses show winning blocks out of seven. Adjacent ABBA/BAAB fresh processes use identical calibrated batch counts, two warmups and three measured rounds; 30 ms target, maximum 256 calls. Every output and guard is checked outside timing. No samples were discarded.

| Variant | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |
|---|---:|---:|---:|---:|---:|---:|
| phased32 | -4.06% (0/7) | -52.00% (0/7) | -5.28% (0/7) | -54.85% (0/7) | -30.29% (0/7) | -60.21% (0/7) |
| packed-b68 | +8.11% (7/7) | -41.93% (0/7) | +7.94% (7/7) | -38.00% (0/7) | -23.21% (0/7) | -42.92% (0/7) |
| swizzled-b64 | +2.57% (6/7) | -50.03% (0/7) | +1.90% (6/7) | -45.66% (0/7) | -30.35% (0/7) | -51.22% (0/7) |

The padded layout has two approximately 8% small-shape wins, retained here. They are insufficient for promotion: the other four shapes regress, and no independent replication or whole-model qualification establishes a useful specialization. The swizzle does not recover broad performance. Do not attribute the padded regression solely to its extra storage.

## Correctness and load limits

216 passing validation executions: three controls × twelve shapes × three patterns × baseline/candidate, with Metal API and shader validation enabled. Every result bit, input immutability and output guard is checked. Finite mixed-scale and signed-zero patterns additionally use an independently rounded CPU adjacent-pair oracle. NaN/infinity/subnormal patterns check compatibility with the existing GPU kernel, not independent IEEE conformance. Timed, calibration and capture runs repeat output checks without validation instrumentation during timing.

Builds, validation, captures, replay profiling and timing run sequentially. Background applications remain open; five-second external CPU samples exclude the benchmark process tree. 100% means one CPU core, not the whole machine. These are monitored screens, not a claim of an entirely idle GPU.

| Variant | Samples | External CPU minimum | Median | Maximum |
|---|---:|---:|---:|---:|
| phased32 | 9 | 159.2% | 181.6% | 279.0% |
| packed-b68 | 8 | 164.8% | 174.9% | 339.6% |
| swizzled-b64 | 9 | 187.9% | 256.7% | 381.6% |

Chrome, WindowServer, metadata indexing and other applications contribute to recorded background activity. Small percentage differences are not a clean acceptance claim. The broad regressions and resource failures are retained rather than retested until they disappear.

No compiler/runtime/numerical policy, cancellation boundary, fault precedence, ownership or budget behavior changes. No full native suite is claimed for these isolated benchmark-only experiments. The large raw captures and binaries remain ignored; small exports, hashes, qualification and all timing samples are retained.

## Next architectural test

Keep the current general kernel. A more useful next control is compile-time specialization of known contraction dimensions: determine whether eliminating generic bounds and unused reduction-root state lowers actual register demand while preserving the exact addition tree. That needs a generic fallback, bounded pipeline caching and whole-model/upload acceptance. It is a hypothesis, not a measured gain. Another address-layout sweep alone is not justified by these results.

## Reproduction and evidence

Use `register_lifetime_screen.py`, `shared_load_screen.py` or `swizzled_load_screen.py` with `--phase prepare --build NEW_DIR --output NEW_DIR/qualification.json`; then `--phase capture` or `--phase measure` with the same build and unused output path. Source and artifact manifests must match. Profile captures through Xcode GUI; stop replay before measuring. Run `python3 native/benchmarks/fair/resource_guided_report.py` to audit preserved local artifacts and regenerate this report.

- [Compiler fields](gpu-resource-guided-gui-2026-10-29.json)
- [All qualification](gpu-resource-guided-qualification-2026-10-29.json)
- [Audited summary and hashes](gpu-resource-guided-2026-10-29.json)
- [Matching baseline export](gpu-resource-guided-baseline-counters-2026-10-29.csv)
- [Earlier baseline and quad resources](gpu-m2-resources-2026-10-29.md)
- [phased32: paired-2026-10-29.json](gpu-resource-guided-phased32-paired-2026-10-29.json)
- [phased32: paired-2026-10-29-load.json](gpu-resource-guided-phased32-paired-2026-10-29-load.json)
- [phased32: counters-2026-10-29.csv](gpu-resource-guided-phased32-counters-2026-10-29.csv)
- [packed-b68: paired-2026-10-29.json](gpu-resource-guided-packed-b68-paired-2026-10-29.json)
- [packed-b68: paired-2026-10-29-load.json](gpu-resource-guided-packed-b68-paired-2026-10-29-load.json)
- [packed-b68: counters-2026-10-29.csv](gpu-resource-guided-packed-b68-counters-2026-10-29.csv)
- [swizzled-b64: paired-2026-10-29.json](gpu-resource-guided-swizzled-b64-paired-2026-10-29.json)
- [swizzled-b64: paired-2026-10-29-load.json](gpu-resource-guided-swizzled-b64-paired-2026-10-29-load.json)
- [swizzled-b64: counters-2026-10-29.csv](gpu-resource-guided-swizzled-b64-counters-2026-10-29.csv)
