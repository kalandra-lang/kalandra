# Overlap encoding with bounded GPU execution

The runtime can submit the first complete, cost-proven chunk while the CPU
encodes later chunks. It still has at most one submitted command per invocation
awaiting acknowledgement. Work limits, shader arithmetic, command boundaries,
and the cancellation/fault check before each subsequent commit are unchanged.
This targets host preparation latency; it does not make matrix arithmetic faster.

`Options::overlappedEncoding = false` retains full pre-encoding as a private
same-binary control. Streaming and non-costed plans retain their existing paths.
Single-command plans finish encoding before submission and need no concurrent
builder/callback protocol.

## Ownership and the last callback access

The CPU builder initially owns the frame through TaskCore's execution lease.
The first early submission adds an external-completion lease. The latter remains
pending until **both** encoding and every submitted GPU access have ended.

A per-progression mutex protects the owner/checkpoint vectors and the admission
state. The builder publishes complete checkpoints; a callback may select one
only after its predecessor acknowledges successfully and the cancellation and
fault checks pass. The selected command is marked running before releasing the
mutex and committing it.

A callback keeps `commandRunning` set until it enters the same locked decision
that records its acknowledgement and either selects the next command or finishes
the invocation. Clearing it in a separate critical section is unsafe: the CPU
builder could finish and signal the external lease between that unlock and the
callback's final frame access. A deterministic negative-control test reproduces
this gap in the first prototype. The production candidate joins those operations
under one lock. No code accesses the frame after handing off or signaling.

If the GPU finishes while the builder is still working and no checkpoint is
ready, the callback returns without signaling. The builder's next publication or
final exit makes the next decision. TaskCore separately prevents another CPU
resume or frame cleanup until the current CPU execution lease returns.

A builder-exit guard handles cancellation, early returns and exceptions. It
marks encoding finished and stops future admission, while any already submitted
command still owns its access until the callback reaches the final handoff.
Unsubmitted command buffers remain inert and are released during teardown.
The existing deferred-error and group-failure precedence remains in force:
a deadline cancels the child task while the group retains `DeadlineExceeded`.

## Parameter resources and admission

CPU encoding must not modify parameter resources already used by the GPU.
Apple's [shared-storage guidance](https://developer.apple.com/documentation/metal/mtlstoragemode/shared)
requires coordinated CPU/GPU access. We do not rely on concurrent writes to
unused offsets of an in-flight shared buffer.

Each overlapping command therefore seals its parameter arena before submission.
Later encoding allocates a different physical backing. Existing parameter slices
retain their sealed parent through acknowledgement. The callback reads completed
fault words before admitting the next command. Separate-buffer controls already
provide distinct resources and need no arena change.

Admission conservatively budgets a full parameter arena for every possible
command. Both the extra backing and the original alignment padding must fit
without consuming the existing 64 KiB safety headroom. If that proof fails, the
runtime retains the original full-pre-encoding policy and its one shared arena.
There is no cache across invocations and no larger work quantum.

A fixed 64×512 fixture verifies both sides: a 15,732,736-byte budget retains one
parameter backing and five commands; 15,794,176 bytes permits five backings and
the same five commands. Every output bit matches. Charged peak payload bytes are
14,733,312 and 14,749,696 respectively, a 16 KiB charged-payload increase, below both limits.
Larger captures are uploaded first so this fixture isolates execution admission
from transient host-upload duplication.

## Qualification

The existing prepared-program fixtures run enabled and disabled controls,
changing captures, concurrent calls, every allocation checkpoint, every held
cancellation/deadline/device-failure boundary, shape/alias/long-chain cases and
complete teardown in optimized, debug, ASan/UBSan and TSan modes.

`EncodingOverlapTests.py` compiles an instrumented copy of the production source;
its gates are absent from shipping libraries. It independently pauses the CPU
builder after the first commit and the completion callback before its final
handoff. Tests require retained resources and an external lease when either
participant remains live. They cover success, cancellation, deadlines, device
failure and allocation failure, and prohibit subsequent submissions after stop.
The callback-gap negative control fails the new lease assertion; the repaired
protocol passes in all four modes.

Whole-block acceptance uses a frozen predecessor and a same-binary policy
control. No numerical tests, builds or profiling overlap timing. Shader binaries
and CPU LLVM must remain unchanged; every output is compared with the frozen
runtime and independent CPU/MPSGraph numerical bounds. Streaming has its own
numerical verification file. Initial screening results are not acceptance.

## Measured scope

Four prepared modes, four deterministic lifetime-gate modes and 27 broader
groups pass. Full-cell frozen comparisons and independent numerical bounds pass
across regular/odd shapes and upload modes. Work and submission counts do not
change. The [complete report](../../benchmarks/results/gpu-encoding-overlap-2026-10-29.md)
retains primary, same-binary, repeat, streaming and longer-batch studies.

The largest resident case improves about 4% consistently; all-upload confirmation
improves 2.13%. Smaller cases are mixed, with roughly 1–3% negative resident
controls. The largest activation-upload confirmation is also negative despite
positive earlier and policy-control runs. Background applications remain active,
so this is not a dedicated idle-machine claim. Treat this as a targeted overlap
policy, not a universal speedup. A separate closed-entry experiment will measure
the larger remaining handoff cost without changing this cancellation contract.
