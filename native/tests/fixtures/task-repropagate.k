fn total() -> float32 ! TaskError:
  try parallel:
    work = spawn compute()
    try await work

fn compute() -> float32:
  data: Tensor[float32, 2] = tensor([1, 2])
  reduceSum(data)

fn main() -> float32:
  result = total()
  retried = result catch fn(error): fail error
  retried catch fn(error):
    match error:
      TaskError.Fault { kind }:
        match kind:
          FaultKind.Allocation: 42
          _: 43
      _: 44
