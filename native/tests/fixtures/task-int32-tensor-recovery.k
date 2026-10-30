fn overflow(values:Tensor[int32,?]) -> Tensor[int32,?]:
  values + 1
fn identity(values:Tensor[int32,?]) -> Tensor[int32,?]:
  values
fn batch(values:Tensor[int32,?]) -> Tensor[int32,?] ! TaskError:
  try parallel:
    work = spawn overflow(values)
    sibling = spawn identity(values)
    first = try await work
    try await sibling
    first
fn probe() -> Tensor[int32,?] ! TaskError:
  original:Tensor[int32,2] = tensor([1,2147483647])
  fallback:Tensor[int32,2] = tensor([0,0])
  result = batch(original)
  recovered = result catch fn(error):
    match error:
      TaskError.Fault {kind:cause,message:detail}:
        match cause:
          FaultKind.Arithmetic: original
          FaultKind.Allocation: original
          _: fallback
      _: fallback
  try parallel:
    work = spawn identity(recovered)
    first = try await work
    again = try await work
    first + (again - original)
fn failedProbe() -> Tensor[int32,?] ! TaskError:
  original:Tensor[int32,2] = tensor([1,2147483647])
  try batch(original)
fn main() -> int32:
  0
