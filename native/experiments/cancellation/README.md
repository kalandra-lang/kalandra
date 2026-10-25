# Cancellation mechanism experiments

These are isolated Darwin ARM64 experiments, not a new language ABI or a
production scheduler implementation. Production cancellation is unchanged.
The source programs are compiled by the existing compiler; a small LLVM tool
then prepares candidate mechanisms in the generated native code.

The main architectural result is that *where cancellation is detected* and
*how control reaches cleanup* are independent decisions. Compiler ownership
rules can make a single task cleanup boundary possible without walking every
native frame. That is a concrete implementation of compiler-generated cleanup
per task, rather than a competing idea.

## Mechanisms

| Mode | Detection | Transfer to cleanup |
| --- | --- | --- |
| `floor` | None: existing unbound code | Normal return |
| `current` | Existing bound token path | Existing private C++ unwinding |
| `entry` | Entry and CFG cycle checks | Direct ledger boundary |
| `cycles` | Recursive call cycles, indirect entries, CFG backedges | Direct ledger boundary |
| `budget` | Local credit counter; acquire token check every 256 visits | Direct ledger boundary |
| `guard` | Poisonable limit fused into existing physical-stack branch; CFG backedge checks for loops | Direct ledger boundary |
| `page` | Read a per-invocation page; revoke access to request cancellation | Fault handler redirects to ledger boundary |
| `async` | Targeted signal; interrupt only generated native PC ranges | Signal handler redirects to ledger boundary |
| `async-unwind` | Same targeted signal | Resume a regular C++ bridge, then unwind |
| `hybrid` | Targeted signal plus checks on returns from runtime helpers | Direct ledger boundary |
| `unwind-poll` | Same placement as `entry` | Regular C++ bridge, then unwind |

`floor` is a lower-bound control, not a cancellation candidate. Entry placement
is deliberately conservative; this pass is not an optimal safepoint-placement
algorithm. All members of a mutual call cycle are covered. CFG backedges also
cover loops after tail-call elimination. The two placements can coincide in
particular programs. Budget credit counts visits, not elapsed time.

## Restricted safety contract

* A reserved callee-saved register, x28, carries experiment state. The entry
  wrapper saves the previous value. Direct transfer restores the saved native
  context with `siglongjmp`; its boundary cost is included in measurements.
* Generated frames own no C++ objects requiring destructors. Native backing is
  admitted to `Invocation`'s ledger. The boundary balances call admission and
  settles that ledger after either normal return or cancellation.
* A signal handler does **not** throw, allocate, free, unwind, or settle a task.
  It checks immutable linked PC ranges and lock-free state, then changes the
  saved PC. Cleanup or unwinding happens after normal signal return.
* Runtime helper functions, allocators, scheduler machinery, and sanitizer
  functions are outside the eligible PC ranges. Their C++ temporaries and
  ownership updates must finish before interruption. The hybrid explicitly
  checks when a helper returns, avoiding dependence on repeatedly sampling
  short native intervals in an allocation-heavy workload.
* Linked function extents are discovered from text symbols, installed as
  relocatable metadata, and checked again after linking. Optimized generated
  boundary wrappers are included: loops may have been inlined into them.
* Whole function extents are useful for abandoning resource-free frames; they
  are **not proof of asynchronous unwind correctness at every instruction**.
  The unwind experiment enables asynchronous unwind tables and frame pointers,
  but passing sampled interruptions does not qualify every prologue/epilogue.
* No foreign callback can appear beneath these generated call chains in the
  tested programs. Production needs an explicit critical-region/callback
  contract; a PC whitelist alone cannot establish that invariant generally.
* Each probe owns its signal handlers, worker, state, and metadata for their
  entire lifetime. Workers are joined before releasing state. This does not
  qualify pooled-worker reuse, task migration, nested cancellation boundaries,
  late-generation interrupts, or coexistence with other signal users.
* No synchronous memory reuse is permitted for submitted GPU borrows. The
  physical Metal test submits work behind an event, settles the CPU invocation,
  verifies its retained lease, releases the GPU event, checks every result, and
  releases the final lease only after GPU completion.

Never transfer across an arbitrary C++ stack, force-kill an OS thread, or treat
an interrupted foreign function as resource-free. Submitted GPU work is allowed
to finish with retained storage. None of these mechanisms provides a real-time
wall-clock cancellation guarantee for an OS thread or a blocking foreign call.

## Reproduce

Requires the built native compiler/runtime and the pinned Homebrew LLVM used by
this repository. Run from the repository root:

```sh
python3 native/experiments/cancellation/qualify.py /tmp/cancellation.json
python3 native/experiments/cancellation/compare.py /tmp/cancellation-fair.json
python3 native/experiments/cancellation/run.py --kind owned --build \
  --cancel --depth 100000000 --repetitions 100 --sanitizer address
python3 native/experiments/cancellation/run.py --kind owned --build \
  --cancel --depth 100000000 --repetitions 50 --sanitizer thread
```

`run.py` accepts `--kind polynomial|branches|euclid|tail|mutual|owned` and
`--sanitizer none|address|thread`. `qualify.py` also tests injected allocation
failures and completion races. Every successful native execution asserts an
empty ledger, balanced allocation/release counts, and the expected runtime
status. Long cancellation tests must report all tasks cancelled; race tests
allow completion or cancellation and verify that only complete results appear.
Worker stacks are 8 MiB for cancellation stress. Physical-stack faults observed
in the initial 512 KiB stress runs are described in the results report.

For the physical Metal check (requires access to the GPU):

```sh
/opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -O2 -fobjc-arc \
  native/experiments/cancellation/GpuLease.mm \
  build/native/native/libkalandra-task-core-optimized.a \
  -framework Metal -framework Foundation -o /tmp/cancellation-gpu
/tmp/cancellation-gpu
```

See [the measured results](../../../benchmarks/results/cancellation-mechanisms-2026-10-25.md)
for performance, latency, sanitizer failures, and the remaining production
qualification requirements. Linux, Windows, ARM64e pointer authentication, and
macOS 26 execution are not qualified by these macOS 27 ARM64 experiments.

The [runtime entry correction](../../../benchmarks/results/cancellation-runtime-entry-2026-10-25.md)
compares a frozen earlier runtime with the current one using identical cleaned
probe objects and generated code. Run `runtime_entry.py OLD_ARCHIVE OUTPUT.json`
after preparing the depth and entry-ablation artifacts. It requires a quiet
machine; build and correctness checks must finish before throughput measurement.
