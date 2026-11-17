# Cooperative GPU kernels

## Accepted source contract

Approved by the project lead on 2026-11-17. Use ordinary Kalandra functions plus `std/gpu`, preserving `spawn … on gpu`, explicit transfers, resident results and the existing numerical/error/ownership contracts. No new kernel keyword or foreign shader language.

Proposed public surface:

- `dispatch(outputShape, groupShape, callback)` constructs a numeric tensor. Both shapes have the same statically known rank (up to eight); output extents may be dynamic, group extents must be positive compile-time constants within physical device limits.
- The callback takes a non-escaping `WorkItem` and returns one numeric element. `globalIndex(axis)` and `localIndex(axis)` return logical i64 indices.
- `item.shared[T](count)` declares fixed-size, numeric, group-local scratch at a collective allocation site. All participating invocations at that site see the same storage; it cannot escape or become a host/worker capture.
- `item.barrier()` synchronizes that workgroup's shared memory. The checker must establish convergence, including helper calls, branches, loops and early exits. Fault/error handling cannot strand peers at a barrier.
- Scratch writes require a proof of disjoint ownership; unresolved races are rejected. Inputs/captures remain read-only. General shared global mutation and atomics are outside this approved API and need a separate proposal.
- Partial groups execute all participating callback lanes, including padding. Only in-domain return values are stored. Out-of-range source reads still fault; empty output domains launch no groups.

### Example

```kalandra
from "std/gpu" import dispatch, WorkItem

fn sample(values: Tensor[f32, ?], index: i64) -> f32:
  n = shape(values)[0]
  position = if index < 0:
    0
  else:
    if index >= n: n - 1
    else: index
  return values[position]

fn smooth(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  n = shape(values)[0]
  return dispatch([n], [256], fn(item: WorkItem) -> f32:
    index = item.globalIndex(0)
    lane = item.localIndex(0)
    tile = item.shared[f32](258)
    tile[lane + 1] = sample(values, index)
    if lane == 0:
      tile[0] = sample(values, index - 1)
    if lane == 255:
      tile[257] = sample(values, index + 1)
    item.barrier()
    return (tile[lane] + tile[lane + 1] + tile[lane + 2]) / f32(3)
  )
```

`[3,6,9,12]` produces `[4,6,9,11]`. This is true cooperative device execution, not serial callback emulation or silently running on CPU.

## Completion boundary

- [ ] Compiler/type checking, race and barrier-convergence diagnostics, typed IR and actual cooperative lowering for Metal, Vulkan and CUDA.
- [ ] Device/shape/scratch/workgroup resource admission, checked arithmetic/indexing, typed errors/fault precedence, bounded execution and cancellation/settlement, including padding lanes and error-before-barrier cases.
- [ ] Independent numerical CPU references for gather/stencil and cooperative tiled/reuse cases; all ten numeric types and logical ranks zero through eight; shape/resource/unsupported-capability diagnostics.
- [ ] Real Metal and Linux Vulkan/CUDA execution: empty/partial/multiple groups, divergent control, scratch ownership and initialization, helper calls, loops/early exits, errors, concurrency, cleanup, isolation and relocation. Windows GPU hardware remains experimental.
- [ ] Honest frozen paired/control/replication performance comparisons against equivalent direct kernels and existing Kalandra paths; preserve regressions and investigate material regressions. No overlapping timing/build/test/profile jobs; monitor host/GPU load.
- [ ] Runnable user documentation, public API coverage and editor support; hosted qualification and accurate PR evidence.

Prerequisite #266 and #279 work has been completed. Portability is explicit: physical limits/capabilities may reject a launch, never silently change its layout or substitute CPU execution. In-flight cancellation is cooperative and resource retirement waits for actual completion; this does not promise arbitrary immediate GPU preemption. Any architectural or contract change discovered during implementation must be documented explicitly.

## Implementation architecture

Existing generated kernels remain on their current numerical/runtime paths. Cooperative dispatch needs a distinct checked operation and launch descriptor: group geometry, padded logical domain, fixed scratch requirements, supported captures and output element type. GPU-placed orchestration emits this dispatch at a graph boundary rather than attempting device-side dynamic parallelism.

WorkItem and shared scratch are scoped compiler capabilities. A structurally similar user record must not forge them. Shared memory is allocated per collective call site per group, not once per invocation; capabilities cannot escape through records, arrays, closures, tasks or results. Ordinary numerical backing and public ByteBuffer storage are not shared scratch.

Safety analysis precedes emission. Track workgroup-uniform versus invocation-varying values, collective control traces, per-phase initialized locations and disjoint write ownership. Finite local-lane enumeration can prove affine/constant indexing and lane-guarded halo writes without trusting an unchecked user promise. Unsupported or exhausted proofs produce diagnostics. Helper expansion, path analysis and static iteration have explicit compiler budgets.

Bounds/arithmetic/application errors must not introduce a per-lane early return before a required collective. Generated cooperative control must preserve a converged barrier schedule, latch failures and retire the entire submitted work before publishing failure or output. This requires its own lowering path; ordinary generated kernels must not pay for cooperative bookkeeping. Cancellation retains current completion boundaries and cannot promise immediate driver-level preemption.

Lower common checked device IR to real workgroup memory and barriers for SPIR-V/Metal and NVPTX/CUDA. Launch admission checks physical workgroup and scratch limits before submission. Partial workgroups execute padding lanes and mask only final output stores; empty output shapes produce no dispatch. Packaging and relocation must retain all generated shader modules and their resource contracts.

## Qualification sequence

1. Freeze the released v0.99.3 compiler/runtime baseline and record actual hardware/toolchain versions. No production arithmetic or existing launch paths change as an incidental consequence.
2. Add checker/IR and independent safety-analysis tests, including deliberately rejected races and divergent barriers.
3. Implement and qualify actual cooperative Metal, Vulkan and CUDA dispatch, including the approved stencil and multidimensional/padded cases.
4. Run targeted neighboring numerical, ownership, task/error and GPU regressions before full platform/package qualification.
5. Collect sequential paired timings, identical-build controls and independent replication with CPU/GPU load admission. Compare equivalent algorithms and preserve adverse results.
6. Complete runnable user documentation, editor/API coverage, source/installed-package/relocation checks, hosted CI and final PR evidence.

Windows GPU packages remain experimental until their separate hardware qualification. Linux remote qualification uses only existing tools and a task-owned directory that is removed after evidence is collected; no system installation or changes to unrelated projects are authorized.
