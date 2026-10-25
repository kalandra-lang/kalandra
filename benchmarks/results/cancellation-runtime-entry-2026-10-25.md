# Runtime entry cost correction

The remaining small-task cost included a production runtime constructor scanning
the environment for `KALANDRA_TEST_FAIL_ALLOC_AT` on every invocation. This is a
test-only fault-injection hook. Resolve it at the first resource reservation
instead, once per invocation. Explicit `failAllocationAt()`, including zero,
overrides the process hook. The test hook now samples the environment at first
reservation rather than construction; process-based injection is preserved.
Also return immediately from an already-settled invocation's cleanup.

Invocation remains 248 bytes, alignment 8. All generated-code ABI offsets remain
unchanged: poll mode 216, cancellation 16, lease depth 224, stack low/high 232/240,
call depth 32. The new flag occupies existing padding. No cancellation, checked
arithmetic, physical-stack checks or ownership checks were removed.

| Non-tail chain depth | Old runtime ns/task | New runtime ns/task | C++ ns/task | Rust ns/task | New / C++ | New / Rust |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 96.65 | 25.00 | 1.81 | 3.70 | 13.78 | 6.76 |
| 100 | 1001.81 | 926.49 | 861.11 | 866.95 | 1.076 | 1.069 |
| 1000 | 9522.82 | 9381.87 | 9021.83 | 9098.63 | 1.040 | 1.031 |
| 10000 | 95424.27 | 95597.40 | 91523.75 | 91774.27 | 1.045 | 1.042 |

Empty-entry cost fell 74%. The 100-level case now meets the 10% target against
both ordinary references in the reported medians. Six of seven block-level
depth-100 comparisons are within 10% for both references; one noisy block
reports 20.3%/22.5% overhead. These are throughput medians, not a worst-case
timing guarantee. The empty case still has material overhead relative
to an ordinary function call; task entry and function call are different units.
The 10000-level old/new difference is approximately 0.2%, not an established
regression or improvement.

Seven randomized mirrored blocks use identical iteration counts and two full
batch warmups per process. Old/new Kalandra executables link the **same freshly
compiled cleaned harness object and unchanged hybrid kernel IR** against the
frozen old or rebuilt runtime archive. Source algorithms, numerical references
and all per-invocation result/status/ledger checks remain. No latency timestamp
is taken in this throughput harness. An initial run overlapped a build and was
discarded; this table uses the subsequent quiet run only.

All 60 single-result checks pass across four implementations, five depths and
three seeds using the exact recurrence oracle. The comparison also passes 100
owned cancellation cases per runtime, and the new runtime passes another 100
each under ASan/UBSan and TSan with empty diagnostic output. Clockless latency
fields are unmeasured. The existing callback/register and PC eligibility
restrictions still apply.

All 44 focused CTest gates pass after rebuilding the affected runtimes and
runtime unit tests: invocation and environment injection, tasks/source tasks,
allocation leases, recursive values, call foundations, fast/tail calls, source
tensor execution, closures, records, strings and recursive source execution.
These cover debug/optimized and available ASan/UBSan/TSan variants; this is not
a claim that the entire repository test suite was rerun.

This changes the production invocation lifecycle, but **does not enable the
experimental hybrid cancellation mechanism in production**. General foreign
callback contracts, task-pool reuse/migration, nested boundaries, handler
coexistence and platform qualification remain outstanding. This single-chain
result does not establish the target across all programs.

- [Samples, verification, commands and hashes](cancellation-runtime-entry-2026-10-25.json)
- [Sanitizer cancellation evidence](cancellation-runtime-entry-sanitizers-2026-10-25.json)
- [Comparison runner](../../native/experiments/cancellation/runtime_entry.py)
- [Earlier harness ablation](cancellation-entry-ablation-2026-10-25.md)

Reproduction requires the existing depth/entry build artifacts and a frozen
pre-change task-core archive:

```sh
python3 native/experiments/cancellation/runtime_entry.py \
  /path/to/pre-change-runtime.a /tmp/runtime-entry.json
```

Apple M2 Max, Darwin ARM64, macOS 27, pinned LLVM 23.1.3_2. Other machines and
platforms have not been measured by this experiment.
