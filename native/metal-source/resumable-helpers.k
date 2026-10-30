fn main() -> float32 ! TaskError:
  try parallel:
    values:Tensor[float32,3] = tensor([1,2,3])
    fallback:Tensor[float32,2] = tensor([10,20])
    factor = recover(values,fallback)
    host = try compute(values,factor)
    reduceSum(host)

fn recover(values:Tensor[float32,?], fallback:Tensor[float32,?]) -> float32:
  outcome = attempt(values)
  outcome catch fn(error):
    match error:
      TaskError.Fault {kind:cause, message:detail}:
        match cause:
          FaultKind.Shape: reduceSum(fallback)
          _: 40
      TaskError.Cancelled: 50
      TaskError.DeadlineExceeded: 60
      TaskError.AdmissionRejected {reason:explanation}: 70

fn attempt(values:Tensor[float32,?]) -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    uploaded = try await transfer(values,gpu)
    preliminary = try await spawn scale(uploaded,1) on gpu
    work = spawn fixed(forward(values))
    sibling = spawn fixed(forward(values))
    first = try await work
    try await sibling
    first

fn compute(values:Tensor[float32,?], factor:float32) -> Tensor[float32,?] ! TaskError:
  try transform(values,factor)

fn transform(values:Tensor[float32,?], factor:float32) -> Tensor[float32,?] ! TaskError:
  try parallel:
    cpuWork = spawn forward(values)
    ready = try await cpuWork
    gpu = devices.gpu()
    input = try await transfer(ready,gpu)
    resident = try await spawn scale(input,factor) on gpu
    try await transfer(resident,devices.cpu())

fn scale(values:Tensor[float32,?], factor:float32) -> Tensor[float32,?]:
  values * factor

fn fixed(values:Tensor[float32,2]) -> float32:
  reduceSum(values)

fn forward(values:Tensor[float32,?]) -> Tensor[float32,?]:
  values
