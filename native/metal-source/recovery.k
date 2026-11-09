fn scale(values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return values * factor

fn fixed(values: Tensor[f32, 2]) -> f32:
  return reduceSum(values)

fn forward(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values

fn main() -> f32 ! TaskError:
  return try parallel:
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    fallback: Tensor[f32, 2] = tensor([10, 20])
    gpu = devices.gpu()
    outcome = parallel:
      uploaded = try await transfer(values, gpu)
      preliminary = try await spawn scale(uploaded, 1) on gpu
      work = spawn fixed(forward(values))
      sibling = spawn fixed(forward(values))
      first = try await work
      try await sibling
      first
    recovered = outcome catch fn(error):
      return match error:
        TaskError.Fault {kind: cause, message: detail}:
          match cause:
            FaultKind.Shape: reduceSum(fallback)
            _: 40
        TaskError.Cancelled: 50
        TaskError.DeadlineExceeded: 60
        TaskError.AdmissionRejected {reason: explanation}: 70
    input = try await transfer(values, gpu)
    resident = try await spawn scale(input, recovered) on gpu
    host = try await transfer(resident, devices.cpu())
    reduceSum(host)
