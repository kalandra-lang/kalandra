fn main() -> Tensor[f32,3] ! TaskError:
  return try parallel:
    values:Tensor[f32,3] = tensor([1,-2,3])
    factor:f32 = -(-2)
    shifted = values -> shift(1)
    cpuWork = spawn shift(shifted,1)
    ready = try await cpuWork
    gpu = devices.gpu()
    resident = try await transfer(ready,gpu)
    gpuOutcome = parallel:
      short:Tensor[f32,2] = tensor([10,20])
      try await spawn fixed(forward(short))
      try await spawn scale(resident,1) on gpu
    forwarded = gpuOutcome catch fn(error): fail error
    safe = forwarded catch fn(error):
      return try parallel:
        try await spawn scale(resident,1) on gpu
    residentAgain = try safe
    outcome = parallel:
      short:Tensor[f32,2] = tensor([10,20])
      try await spawn fixed(forward(short))
    adjustment = outcome catch fn(error):
      return match error:
        TaskError.Fault {kind:kind, message:message}:
          match kind:
            FaultKind.Shape:
              factor:f32 = 3
              factor
            _: 99
        _: 99
    work = spawn scale(residentAgain,factor+adjustment) on gpu
    result = try await work
    host:Tensor[f32,3] = try await transfer(result,devices.cpu())
    final = try parallel:
      values:Tensor[f32,3] = tensor([1,1,1])
      host + values
    final -> shift(-1)

fn shift(values:Tensor[f32,?], amount:f32) -> Tensor[f32,?]:
  return values + amount

fn scale(values:Tensor[f32,?], factor:f32) -> Tensor[f32,?]:
  return values * factor

fn forward(values:Tensor[f32,?]) -> Tensor[f32,?]:
  return values

fn fixed(values:Tensor[f32,3]) -> f32:
  return reduceSum(values)
