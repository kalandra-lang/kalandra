# Guarded float32 matrix epilogues

Prepared numerical programs may fuse a matrix operation with a following
compiler-proven, non-faulting float32 map. The map can use the matrix value and
one additional resident tensor. An intervening `EqualShape` is admitted only
when its outcome is already known to pass from the matrix's predicted output
shape and the other tensor's current metadata.

The compiler puts the matrix value first in the map's parameter ABI. It does
not reorder expression operands or source evaluation. This permits the existing
canonical epilogue generator to substitute the completed float32 matrix value
for parameter zero. A subtraction with the matrix on its right therefore stays
`other - matrix`. Normal maps avoid generating unused matrix pipeline profiles.

The runtime requires a prepared definition, the arithmetic proof, float32
capture metadata, a supported direct kernel, a single map consumer, and ready
resident captures with the expected shape. The matrix result may additionally
have the one use in the intervening guard. Missing values, failing shapes,
other uses, broadcasts, unsupported types, low memory headroom and unsupported
kernels retain staged execution. A failed guard is never raised early: an
in-flight matrix device failure retains precedence over that later shape fault.

Fused captures are read from this invocation's value table, rather than the
immutable definition's placeholder buffers. Finishing the pair retires uses
from the matrix, optional guard and map exactly once. Batched retirement retains
all backing until device acknowledgement. Original inputs remain immutable;
no user-visible ownership or mutation rule changes.

Both computational operations count toward the estimated 64 Mi-work cancellation
quantum. Compiler-costed plans use [work-bounded progression](gpu-work-bounded-progression.md);
other paths also retain the four-logical-stage cap. The existing exception for a single
oversized fused dispatch remains: the map's estimated work is charged with the
matrix, and no unrelated work is appended. Cancellation cannot interrupt a
running Metal dispatch. Fusion changes physical checkpoint placement within
that contract: the large transformer block has six submissions rather than
seven because its final residual now shares the down-projection dispatch.

Products still round separately, use the existing adjacent-pair reduction tree,
and become a float32 value before the map. No FMA contraction, reassociation,
precision change, vendor numerical dependency or device-side cancellation word
is introduced. Empty matrices keep staged validation.

Qualification compares staged and fused IEEE bit patterns with and without the
guard, checks noncommutative operand order and compiler-generated source entry,
and exercises failed/empty shapes, prior device failure, cancellation, aliases,
allocation failures, concurrent invocations and every submission boundary in
optimized, debug, ASan/UBSan and TSan modes. Performance acceptance uses separate
uninstrumented counterbalanced runs, with original generated programs relinked
against the new runtime as an additional compatibility check.
