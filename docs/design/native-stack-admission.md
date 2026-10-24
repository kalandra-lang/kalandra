# Bounded native frames for long tensor functions

GPU-HOST-001 is repaired by reducing generated CPU code per function. The CPU
stack budget remains **16 KiB**, source functions retain ordinary call admission,
and the GPU type/operation subset does not change.

The failure appeared in the existing 29- and 40-tensor numerical examples. Their
GPU argument layouts were supported, but shared CPU bridge helpers exceeded the
conservative native stack estimate. Both complete examples include CPU calls;
removing unused exports alone would not restore them.

## Lowering changes

After installing checked allocation, fault and loop-poll calls, the CPU backend
simplifies LLVM IR with instruction combining, early common-subexpression
elimination and dead-instruction removal. It uses the same pinned ARM64 data
layout as storage and stack admission. This removes redundant tensor descriptor
construction and extraction. Applying the passes after checked ABI conversion
keeps failure exits and cancellation observable during simplification. Function
pointers and closure entries are remapped to their final checked signatures
before these passes run. Strict arithmetic order is retained. The current [native call optimizer](checked-call-optimization.md) may inline source calls within the same checked frame budget; this historical repair did not depend on that optimization.

Functions containing at least sixteen float32 tensor unary/binary operations
lower those operations through compiler-generated helpers. This threshold is an
internal code-generation choice, not a language limit. Operands still evaluate
in their original order. Each helper performs the existing elementwise operation,
shape checks and immutable result allocation, with the original source location.
Helpers are not inlined into the caller and pass through the normal checked ABI:
allocation faults, cancellation polling, balanced call admission and return,
and the same conservative per-function stack check. They receive no special
stack exemption. Existing compiler-owned SIMD matmul leaf policy is unchanged.

Both simplification and helper boundaries are present in emitted IR even for
`-O0`, so acceptance does not depend on a later optimizer deciding to reuse
registers or stack slots. The estimator still counts the instructions actually
emitted, arguments, outgoing argument copies and fixed allocations. Dynamic or
scalable stack storage remains rejected. Oversized ordinary functions still
reject without replacing existing output; deep recursion retains checked
`StackExhaustionFault` and invocation cleanup.

This increment outlines float32 elementwise operations. It does not claim general
large-function acceptance for every numeric type or arbitrarily large argument
packets. Memory budgets and physical stack admission remain applicable. Extra helper
calls and function-boundary ownership lowering may affect performance and
allocation counts; no speedup claim is made.

## Qualification

The two original positive GPU CLI gates build both source programs in normal,
optimized, ASan/UBSan and TSan host modes, run with one/two/four workers, and retain
numerical output, shape/residency diagnostics, argument-layout checks, strict
shader artifacts and application relocation. Normal and sanitized compiler
capability checks now require successful source emission for those examples.

The existing direct generated-entry GPU harnesses exercise numerical corpora,
CPU allocation-failure unwinding, Metal preparation failures, cancellation,
deadlines, cached results, byte budgets and ownership after scheduler release.
Host sanitizers do not instrument GPU shader instructions.

Dedicated CPU tests combine forty outlined unary/binary operations, check helper
emission and call-admission markers, and verify ordinary and empty inputs in all
four host modes, including a returned function-pointer choice whose targets
fold to the same named function. Recursive-callable regression gates retain all numeric widths,
owned results/errors, expected fault handling, oversized stack rejection with
output preservation, and deep-recursion cleanup. Record, String-field and enum
regressions cover the shared ABI simplification outside numerical code.

See the [qualification record](../../native/metal-proof/evidence/gpu-host-stack-repair-2026-10-20.json)
for exact checks, compiler builds, artifacts and remaining limits.
