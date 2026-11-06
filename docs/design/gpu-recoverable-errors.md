# Recoverable application errors on the GPU

Design for [#283](https://github.com/kalandra-lang/kalandra/issues/283).

The GPU should implement the same typed outcome as a local CPU worker. In
`T ! E`, exactly one of the success value and the application error is active.
A normal return produces success. `fail` produces an error, `try` propagates it
to its lexical boundary, and `catch` runs only for an expected failure.
A handler may recover, rethrow, or replace the error. Family unions preserve
the original family and variant through widening and narrowing.

An application error escaping a GPU task is a completed worker outcome.
Awaiting the task exposes `T ! (E | TaskError)` and repeated awaits observe the
same outcome. Catching the application error inside the group leaves siblings
running. Propagating it out of the group uses the existing primary outcome
reservation and settlement rules. A recorded execution failure wins over a
later reservation. Once the reservation succeeds, the cancellation used to
drain siblings cannot replace the application error. A competing deadline
or cancellation follows the same scheduler rules as for CPU workers.

Checked arithmetic, bounds, shape and allocation faults continue through the
execution fault path. These faults bypass a device-local application handler.
The host supervisor exposes task failures through the existing `TaskError`
variants. `TaskError` is a host scheduling channel at this boundary. Device
functions can declare application error families, including unions of those
families. Device code does not create host scheduling outcomes.

## Values and ownership

Admit payload fields through the GPU value layout rules, recursively checking
the active nominal variants and all declared field types at compile time.
Numeric and Bool fields, admitted records and enums, and admitted owned arrays
and tensors use the same layout and ownership rules as ordinary GPU values.
Host-only fields such as strings and task handles require an explicit
diagnostic identifying the error type and field. Fields requiring transfer
back to the host must also pass the existing result transport rules.

Keep the outcome discriminant separate from execution status. Use typed
outcomes in device continuation slots and recursive frames, with the active
payload visible to the device collector. Catch and propagation branch on the
outcome discriminant. Widening and narrowing translate the nominal tag and
active fields. Abandoning a partial result or an in-progress collection
follows the existing continuation and arena cleanup paths.

Export an escaping outcome through bounded result transport. Validate its
discriminant and selected payload before publishing a host observation. Do
not read or retain inactive data. A malformed result or failed materialization
is an execution fault. A cancellation or deadline during readback follows the
existing task checkpoint and settlement rules. Imported aliases and newly
allocated backing use the existing result ownership rules. Release temporary
storage on every terminal path, including interrupted export and partial
materialization.

## Implementation and verification

The shared GPU continuation planner already represents `Fail`, `Catch`, and
`Try`. Teach the shared shader path to consume their typed outcomes, including
direct and indirect calls. Preserve handler boundaries when calls inline, and
preserve active outcome roots when calls use device frames. Connect the
boundary result to the host scheduler's existing application-error channel.

Compare Metal, Vulkan and CUDA with CPU outcomes for recovery, nested and
recursive propagation, handler replacement, owned payloads and cached
observations. Exercise failure precedence, concurrent tasks, interrupted
transport and bounded cleanup. Verify runnable examples and compiler
diagnostics. Qualify available physical GPUs and record the Windows hardware
qualification limit separately.
