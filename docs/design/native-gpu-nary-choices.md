# Multi-source fused GPU maps

A pure tensor choice can now read more than two tensor sources:

```kalandra
fn select(a: Tensor[float32, 3], b: Tensor[float32, 3], c: Tensor[float32, 3], factor: float32) -> Tensor[float32, 3]:
  if factor > 0:
    if factor > 1:
      a * factor
    else:
      b / factor
  else:
    c + factor
```

The arms form one elementwise calculation with one output tensor. GPU functions
can use more sources in their arithmetic and nested choices. Operand counts follow
the existing compiler, native-stack, capture and device resource budgets rather
than a fixed two-source ceiling. The qualification includes a forty-source
resident map with a reduction-driven predicate and a Tier 2 resource table.

[Fixed and checked dynamic shape proofs](native-gpu-dynamic-choices.md) still
apply to every source and tensor operation. Independent unknown shapes remain
rejected. Branch-local allocation, reductions, matrix products, dispatch and
fallible operations remain outside the pure fused-arm subset. The compiler may
evaluate both arms before selecting result bits.

## Graph representation and ownership

Ordinary elementwise entries already carry a variable-size read table. Resident
graph maps now also carry an explicit sorted list of source indices. Matrix
products retain their two operands. Existing one/two-source graph descriptors
remain valid; an explicit map list names every source used by a larger fused map.

The runtime validates each list before scheduling GPU work: indices must be
strictly increasing, unique and refer to original inputs or earlier results.
The descriptor anchors must match the first and last listed source. Full shapes,
ranks, residence, device identity and the shader's complete tensor/scalar binding
signature must agree. Matrix stages reject map operand lists.

Capture copies all lists into storage owned by the task graph and includes those
bytes in its argument budget. Descriptors are rebound to that owned storage;
the caller's arrays may be destroyed or changed after submission. The graph
object cannot be copied, preserving the storage referenced by those spans.

Every listed source participates in use counting and last-use reclamation,
including middle operands that are neither descriptor anchor. Checked use counts
reject overflow. Completion retires each explicit reference once; cancellation
and failed preparation release the owned metadata and captured buffers through
the existing task lifetime path. Binding and broadcast classification use the
full list length, so reduced scalars follow all tensor inputs in the resource table.

## Qualified composition

The checked cases include resident maps driven by reductions, retained
intermediates used as middle operands, and a map feeding multiple matrix products.
[Single-matmul operand maps](native-gpu-operand-choices.md) now also accept
different-source choices. Broader ranks and GPU numeric widths remain separate work.
Straight-line graph arithmetic retains its existing stage decomposition; this
step expands the source set within a fused map.

The [fixture](../../native/metal-source/nary-choices.k),
[device checks](../../native/metal-source/NaryChoiceTests.cpp),
[source/CLI checks](../../native/metal-source/VerifyNaryChoices.cmake) and
[qualification record](../../native/metal-proof/evidence/gpu-nary-choices-2026-10-21.json)
cover numerical results, lifetime, malformed tables and release-package relocation.
This is correctness qualification, not a performance claim.

A separate compile-time optimization item tracks pathological MLIR bufferization
of deep tensor-valued conditional chains in CPU shadow functions. Removing the
map source ceiling does not remove compiler analysis or native stack admission
limits; the roadmap records that follow-up.
