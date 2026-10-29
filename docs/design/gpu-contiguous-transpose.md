# Contiguous GPU transpose views

A rank-two tensor with a singleton dimension has the same contiguous byte order
after transposition. Empty matrices likewise need no device copy. Numerical
programs can therefore change the shape descriptor while sharing the existing
immutable Metal allocation. General matrices keep their existing transpose
kernel; no arithmetic or matrix profile changes.

The runtime checks the original format, rank, dimensions, count, registry and
device before taking this path. Unsupported bindings continue through the
existing validation path. The metadata allocation remains fallible and covered
by allocation injection. `Options::contiguousTranspose = false` provides a
private same-binary control.

## Ownership and mutation

Each view has separate shape metadata and retains the original physical backing.
No CPU pointer to private device data is exposed. External inputs keep their
non-reusable origin. Internal views inherit their source origin, and pointwise
reuse checks both descriptor identity and shared physical allocation identity.
A live differently shaped view therefore prevents destructive reuse of its
backing. An inversion followed by a pointwise update cannot modify an earlier
value still needed by the program.

Views do not submit GPU work or add a fault-word slot. An all-view program can
complete without any command buffer or external completion lease, including
when conservative progression admission reserved command credits. The existing
per-instruction cancellation check still runs. If earlier device work is
pending, its acknowledgement and fault/cancellation checks still precede result
publication. Eliminating the copy removes that copy's possible device-failure
point; it does not weaken checks for submitted work.

The planner's existing resource estimates remain conservative. It does not
spend the saved bytes on larger work quanta. Pure one-row transformer attention
uses 11 physical dispatches instead of 12 in the work-bounded path. In streaming,
removing the logical transpose shifts the four-slot window and can prevent a
later ReLU fusion, so its physical dispatch count remains 12. Streaming is
measured separately rather than assuming it shares the same gain.

## Verification

Prepared-program checks cover singleton rows and columns, 1×1, empty and general
matrices; nine-stage all-view programs; signed zeros, subnormals, infinities and
NaN payloads; and a live internal alias across two transposes and a pointwise
update. All-view paths assert zero submissions, zero dispatches and unchanged
physical bytes. Source inputs, output shapes and every output bit are checked.

Full transformer tests include enabled/disabled controls, separate parameter
buffers, streaming fallback, changed captures, concurrent calls, every injected
allocation failure, every held command cancellation/deadline/device-failure
boundary, and complete teardown. Generic transform tests exercise all ten numeric
types. Their held-completion cases explicitly disable views so they continue to
exercise actual GPU work; pre-cancel and ordinary cases retain the new path.

This optimization follows the rejected
[general transposed-matrix kernel experiments](../../benchmarks/results/gpu-transpose-matmul-profiles-2026-10-29.md).
Those experiments improved small isolated cases but regressed larger ones and
are not part of production.

The [acceptance report](../../benchmarks/results/gpu-transpose-view-2026-10-29.md)
retains primary, same-binary, repeated, streaming and confirmation timings,
independent numerical bounds, all qualification logs and source/artifact digests.
The reliable structural gain is eliminating the copy; measured one-row resident
improvements are modest. Unaffected-shape regressions and positive control
excursions are included, without claiming a general matrix speedup.
