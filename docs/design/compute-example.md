# Kalandra CPU GPU and remote application example

Accepted integration walkthrough — 2026-10-15. This uses the accepted [core](core.md), [tensor](tensors.md), and [placement](placement.md) rules. The fresh compiler does not implement this syntax yet.

Compute a matrix projection on the GPU, send its result to a remote CPU executor for summarization, and compare the answer with a local CPU calculation. All three use the same float32 numerical rules. This small input makes the answer easy to check; it is not a workload where distribution would be faster.

## Complete calculation

```kalandra
type Report:
  local: float32
  distributed: float32

fn project(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2]:
  matmul(samples, weights)

fn summarize(values: Tensor[float32, ?, 2]) -> float32:
  reduceSum(values)

fn reference(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> float32:
  summarize(project(samples, weights))

fn main() -> Report ! TaskError:
  samples: Tensor[float32, 2, 3] = tensor([
    [1, 2, 3],
    [4, 5, 6],
  ])
  weights: Tensor[float32, 3, 2] = tensor([
    [1, 2],
    [3, 4],
    [5, 6],
  ])

  try parallel:
    gpu = devices.gpu()
    analysis = devices.remote("analysis")

    baseline = spawn reference(samples, weights) on devices.cpu()
    inputUpload = transfer(samples, gpu)
    weightUpload = transfer(weights, gpu)

    deviceInput = try await inputUpload
    deviceWeights = try await weightUpload
    projection = spawn project(deviceInput, deviceWeights) on gpu
    deviceOutput = try await projection

    remoteInput = try await transfer(deviceOutput, analysis)
    summary = spawn summarize(remoteInput) on analysis

    localValue = try await baseline
    distributedValue = try await summary
    Report {
      local: localValue,
      distributed: distributedValue,
    }
```

The entry point returns a typed Report; printing, exit codes, and application entry-point conventions remain a tooling decision. All functions are shown together because module/export syntax for the fresh language has not been settled. These are source definitions, not a claim that a remote server automatically receives them.

The analysis deployment must contain a compatible compiled summarize entry and its tensor/error contracts. It does not need project or reference. The local build needs CPU code for reference and its callees, GPU code for project, and a trusted remote entry contract for summarize. The remote CPU implementation of summarize follows the same reduction contract as its local use.

## Data movement and concurrency

| Step                | Where work runs    | Data movement                                    |
| ------------------- | ------------------ | ------------------------------------------------ |
| Construct inputs    | Local CPU          | Two host tensors                                 |
| Start reference     | Local CPU executor | Same-host managed values; no network/device copy |
| Submit both uploads | Transfer runtime   | Samples and weights to the selected GPU          |
| Run project         | GPU                | Read device tensors; create device result        |
| Transfer result     | Transfer runtime   | Projected tensor to the analysis deployment      |
| Run summarize       | Remote CPU         | Read remote tensor; return one float32 scalar    |
| Complete Report     | Local CPU          | Two observed scalar task outcomes                |

The reference task and uploads are submitted before any await, so they may overlap within scheduler limits. Both uploads are submitted before waiting for either. Projection depends on both; remote summarization depends on projection and its transfer. This is a dependency chain, not a claim that every step runs simultaneously.

GPU-to-remote transfer names the logical source and destination. A backend may stage through host memory; no direct GPU/network transport is promised. Staging is internal to this explicit transfer and retains the same float32 values. If the first backend cannot support that route, it must report an unsupported transfer rather than invent an implicit copy during spawn. An explicit two-hop alternative is:

```kalandra
hostOutput = try await transfer(deviceOutput, devices.cpu())
remoteInput = try await transfer(hostOutput, analysis)
```

These alternatives have the same logical values, but may differ in storage use and transfer support. Neither requires a host download merely to await the completed GPU tensor. The remote summary is a scalar, so its four-byte numerical value belongs in the task outcome rather than requiring another tensor transfer.

## Expected answer

Projection produces:

```text
[[22, 28],
 [49, 64]]
```

The fixed balanced reduction computes `(22 + 28) + (49 + 64)`, giving float32 163. All values in this example are exactly representable, so Report contains local 163 and distributed 163. Larger floating-point examples must be checked against the accepted rounding/reduction contract, not just a permissive tolerance that could hide a different algorithm.

## Errors and cleanup

Every spawned invocation and transfer is observed on the successful path. Propagating an error exits through the parallel group's settlement boundary. Already observed outcomes remain settled; unfinished descendants are cancelled and joined before the group publishes its error.

| Failure                                          | Required behavior                                                                 |
| ------------------------------------------------ | --------------------------------------------------------------------------------- |
| No suitable GPU                                  | Return a typed placement error; do not silently run the GPU task on CPU           |
| One upload fails                                 | Cancel/settle the other upload and reference; reclaim unpublished partial data    |
| GPU computation faults                           | Fail the group; retain buffers until device work is confirmed finished            |
| Remote schema or entry mismatch                  | Reject dispatch under the placement error contract                                |
| Remote upload succeeds but task submission fails | Release the retained remote value; lease expiry handles client loss               |
| Remote response is lost                          | Report failure; do not automatically rerun or claim the operation did not execute |
| Parent is cancelled                              | Stop admission and settle local, device, and remote descendants                   |

TaskError includes the accepted placement outcomes; their exact payload fields and wire representation remain unfinished API work. No catch-all handler here replaces failures with a plausible number. An application can handle the settled main result explicitly under the accepted try/catch model.

There are no mutable user buffers or manual frees. Samples and weights remain usable on the CPU while their immutable device copies are read. Group ownership governs live tasks; managed ownership governs completed tensors. Cleanup cannot recycle in-use storage just because a handle went out of scope or cancellation was requested.

## Bounded batches

For many independent host-resident projections, use a bounded helper rather than creating an unbounded task list:

```kalandra
fn batchTotals(
  batches: [Tensor[float32, ?, 3]],
  weights: Tensor[float32, 3, 2],
) -> [float32] ! TaskError:
  try parallelMap(
    batches,
    fn(samples): reference(samples, weights),
    ParallelOptions {
      target: devices.cpu(),
      maxInFlight: 4,
    },
  )
```

The callback captures an immutable host tensor and runs on the local CPU. The compiler validates its capture and concrete parameter/result types. The helper owns its group, preserves input order in the returned array, and admits at most four unfinished invocations, subject to tighter runtime limits. Nested groups remain under the caller's cancellation tree.

Changing only target to GPU would not make this correct: inputs and captured weights are host-resident. Changing it to remote would additionally require a deployable callback entry and explicit remote capture handling. The first remote placement model does not ship this closure. Preparing device-resident batches, keeping shared weights resident, and coordinating transfers require an explicit orchestration function. Do not pretend parallelMap removes those decisions.

This finite collection API retains the input and final output arrays. Streaming and byte-bounded buffering are separate work.

## Friction exposed by the example

The model is coherent, but explicit transfer and observation produce several bindings. Keep the fully explicit baseline first. Do not hide the cost with automatic tensor uploads or add punctuation before real applications show which composition helper is useful.

The following contracts must be made precise before their implementation:

- A common target capability type for selectors and APIs; final public spelling follows the generics/resource API work.
- Residency requirements on ordinary function boundaries, including diagnostics when an unknown-location tensor reaches a host operation. Device location stays out of Tensor's mathematical type, but cannot disappear from callable contracts.
- Remote deployment entry selection, exact TaskError payloads, schema identity, authentication, and retained-data leases.
- The supported transfer routes for the first backend, including cancellation and host staging.
- Build-time GPU function eligibility and strict numerical capability checks.

These are tracked gaps, not reasons to introduce source lifetime annotations, implicit transfers, or arbitrary closure upload. Resolve each contract when its implementation stage needs it.

## Next implementation boundary

Begin with one native CPU calculation: construct the two tensors, call reference, and produce float32 163. It needs no scheduler, GPU, network, or external service. Establish the fresh grammar, typed IR, shape checking, and native lowering around that case before adding execution boundaries.

Then add local structured tasks and verify their supervision/cleanup. Add one explicit GPU transfer/dispatch path after the CPU semantics are tested. Add remote execution after its deployment/storage contract is specified. Preserve the same source calculation and numerical evidence at every stage.

Acceptance of this walkthrough does not approve an unimplemented runtime API by implication. Review the [first native CPU compiler milestone](cpu-milestone.md) for supported grammar, diagnostics, expected output, and implementation gates; the full distributed program is its longer-term integration example.
