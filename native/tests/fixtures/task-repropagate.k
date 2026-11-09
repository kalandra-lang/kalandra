fn total() -> f32 ! TaskError:
  return try parallel:
    work = spawn compute()
    try await work

fn compute() -> f32:
  data: Tensor[f32, 2] = tensor([1, 2])
  return reduceSum(data)

fn main() -> f32:
  result = total()
  retried = result catch fn(error): fail error
  return retried catch fn(error):
    return match error:
      TaskError.Fault { kind }:
        match kind:
          FaultKind.Allocation: 42
          _: 43
      _: 44
