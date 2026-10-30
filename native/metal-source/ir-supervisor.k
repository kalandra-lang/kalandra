fn main() -> Tensor[float32,3] ! TaskError:
  try parallel:
    values:Tensor[float32,3] = tensor([1,-2,3])
    factor:float32 = -(-2)
    shifted = values -> shift(1)
    cpuWork = spawn shift(shifted,1)
    ready = try await cpuWork
    gpu = devices.gpu()
    resident = try await transfer(ready,gpu)
    gpuOutcome = parallel:
      short:Tensor[float32,2] = tensor([10,20])
      try await spawn fixed(forward(short))
      try await spawn scale(resident,1) on gpu
    forwarded = gpuOutcome catch fn(error): fail error
    safe = forwarded catch fn(error):
      try parallel:
        try await spawn scale(resident,1) on gpu
    residentAgain = try safe
    outcome = parallel:
      short:Tensor[float32,2] = tensor([10,20])
      try await spawn fixed(forward(short))
    adjustment = outcome catch fn(error):
      match error:
        TaskError.Fault {kind:kind, message:message}:
          match kind:
            FaultKind.Shape:
              factor:float32 = 3
              factor
            _: 99
        _: 99
    work = spawn scale(residentAgain,factor+adjustment) on gpu
    result = try await work
    host:Tensor[float32,3] = try await transfer(result,devices.cpu())
    final = try parallel:
      values:Tensor[float32,3] = tensor([1,1,1])
      host + values
    final -> shift(-1)

fn shift(values:Tensor[float32,?], amount:float32) -> Tensor[float32,?]:
  values + amount

fn scale(values:Tensor[float32,?], factor:float32) -> Tensor[float32,?]:
  values * factor

fn forward(values:Tensor[float32,?]) -> Tensor[float32,?]:
  values

fn fixed(values:Tensor[float32,3]) -> float32:
  reduceSum(values)
