# Independent GPU projection scheduling

The strict output-reuse tile screens did not qualify a faster FFN kernel. The
next experiment exposes independent Q/K/V work to the GPU without adding more
outputs or reduction roots per thread. It retains register16's 16×16 tile,
256 threads, 8 KiB threadgroup storage and adjacent-pair arithmetic.

Two mechanisms need separate controls:

- One batched dispatch selects independent weight/output buffers using the
  second threadgroup coordinate. No tensor concatenation or packing is needed.
- Three unchanged dispatches share a concurrent encoder. This may expose the
  same available work while retaining the existing shader ABI.

Compare each with both three separate encoders and three ordinary dispatches
in one serial encoder. The latter separates encoder setup from scheduling and
pipeline changes. These isolated screens cannot establish whole-program gains.
A stronger result on small grids is compatible with improved utilization, but
is not a direct occupancy measurement or proof of register spilling.

## Evidence boundary

`gpudebug` captured the original FFN contraction and identified its register16
entry. Its `profile run` rejected this M2 Max because that tool requires M3 /
A17 Pro or later. No register counts, spills or occupancy were measured. This
is a limit of that profiling path, not a statement that all Instruments
occupancy interfaces are unavailable on M2. Keep the captured diagnostic
separate from unprofiled acceptance measurements.

## Runtime promotion gate

First replicate the isolated result, then prototype the winning mechanism in a
copied runtime. Start with the existing closed synchronous runner admission
proof. Preserve the ordinary cancellable path and its current work checkpoints.
Do not turn a source-level parallel construct into an indivisible command.

A candidate matcher must prove that the instructions are independent compatible
float32 matrix operations. Their operands must already exist before the group;
none may consume another grouped result. Initial scope should use identical
shapes, the same left operand and existing register16 admission. Preserve each
logical result and all downstream uses; do not change the public function ABI.

Allocate and account for each result through the existing budget machinery.
Retain inputs, outputs and parameter storage until GPU completion. If an encoder
is kept open across logical instructions, all failure and flush paths must close
it before submission; partially prepared groups must preserve earlier fault
precedence. Never leave encoded commands referring to retired storage.

Keep the existing path for incompatible shapes, old shader libraries, tight
resource budgets and absent closed-region proof. Do not weaken numerical checks
or alter contraction order to recover performance. Requalify full graph outputs,
odd/inactive shapes, uploads, repeated/concurrent invocations, low budgets,
allocation/device failures, aliasing and ordinary cancellation/fault precedence.
Run frozen paired whole-transformer controls and independent replication before
promoting a production policy. Keep unfavorable outcomes.

## Completed screen

The [isolated three-projection study](../../benchmarks/results/gpu-qkv-dispatch-2026-10-29.md)
passes 162 validation cases. Batched dispatch lowers elapsed time by 0.7–56.6%
against the serial-encoder control, depending on shape. Three concurrent unchanged
dispatches improve isolated timings by 8.0–49.4%. These are preallocated resident
kernel screens, not language or full-model speedups.

The [copied-runtime whole-model gate](../../benchmarks/results/gpu-independent-dispatch-2026-10-29.md)
does **not** qualify concurrent scheduling. All 117 numerical cases pass, but
frozen and same-binary controls retain regressions and large timing swings,
including inactive shapes. Command-duration diagnostics put most of the large
penalty on the GPU side; separate parameter buffers do not eliminate it. A
read-only input/parameter declaration control passes 234 additional full-output
checks and changes reflected access modes as intended, but does not reliably
recover performance. No register/spill or exact hardware-cause conclusion follows.

Production remains unchanged. The one-grid batched shader needs its own full-model
integration; it must not inherit the isolated result or the concurrent prototype's
numerical qualification as acceptance. Complete the runtime promotion gates above
before any production change. The 1024-wide full-model screen hits the existing
64 MiB budget in both baseline and candidate; the failed run is retained.
