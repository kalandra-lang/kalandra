fn overflow(values:Tensor[i32,?]) -> Tensor[i32,?]:
  return values + 1
fn identity(values:Tensor[i32,?]) -> Tensor[i32,?]:
  return values
fn batch(values:Tensor[i32,?]) -> Tensor[i32,?] ! TaskError:
  return try parallel:
    work = spawn overflow(values)
    sibling = spawn identity(values)
    first = try await work
    try await sibling
    first
fn probe() -> Tensor[i32,?] ! TaskError:
  original:Tensor[i32,2] = tensor([1,2147483647])
  fallback:Tensor[i32,2] = tensor([0,0])
  result = batch(original)
  recovered = result catch fn(error):
    return match error:
      TaskError.Fault {kind:cause,message:detail}:
        match cause:
          FaultKind.Arithmetic: original
          FaultKind.Allocation: original
          _: fallback
      _: fallback
  return try parallel:
    work = spawn identity(recovered)
    first = try await work
    again = try await work
    first + (again - original)
fn failedProbe() -> Tensor[i32,?] ! TaskError:
  original:Tensor[i32,2] = tensor([1,2147483647])
  return try batch(original)
fn main() -> i32:
  return 0
