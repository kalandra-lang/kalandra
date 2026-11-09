fn main() -> f32 ! TaskError:
  return try parallel:
    values:Tensor[f32,3] = tensor([1,2,3])
    fallback:Tensor[f32,2] = tensor([10,20])
    factor = recover(values,fallback)
    host = try compute(values,factor)
    reduceSum(host)

fn recover(values:Tensor[f32,?], fallback:Tensor[f32,?]) -> f32:
  outcome = attempt(values)
  return outcome catch fn(error):
    return match error:
      TaskError.Fault {kind:cause, message:detail}:
        match cause:
          FaultKind.Shape: reduceSum(fallback)
          _: 40
      TaskError.Cancelled: 50
      TaskError.DeadlineExceeded: 60
      TaskError.AdmissionRejected {reason:explanation}: 70

fn attempt(values:Tensor[f32,?]) -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    uploaded = try await transfer(values,gpu)
    preliminary = try await spawn scale(uploaded,1) on gpu
    work = spawn fixed(forward(values))
    sibling = spawn fixed(forward(values))
    first = try await work
    try await sibling
    first

fn compute(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?] ! TaskError:
  return try transform(values,factor)

fn transform(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?] ! TaskError:
  return try parallel:
    cpuWork = spawn forward(values)
    ready = try await cpuWork
    gpu = devices.gpu()
    input = try await transfer(ready,gpu)
    resident = try await spawn scale(input,factor) on gpu
    try await transfer(resident,devices.cpu())

fn scale(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?]:
  return values * factor

fn fixed(values:Tensor[f32,2]) -> f32:
  return reduceSum(values)

fn forward(values:Tensor[f32,?]) -> Tensor[f32,?]:
  return values
