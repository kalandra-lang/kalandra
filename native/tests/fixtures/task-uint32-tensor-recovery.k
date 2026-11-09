fn overflow(values:Tensor[u32,?]) -> Tensor[u32,?]:
  return values + 1
fn identity(values:Tensor[u32,?]) -> Tensor[u32,?]:
  return values
fn batch(values:Tensor[u32,?]) -> Tensor[u32,?] ! TaskError:
  return try parallel:
    work = spawn overflow(values)
    sibling = spawn identity(values)
    first = try await work
    try await sibling
    first
fn probe() -> Tensor[u32,?] ! TaskError:
  original:Tensor[u32,2] = tensor([1,4294967295])
  fallback:Tensor[u32,2] = tensor([0,0])
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
fn failedProbe() -> Tensor[u32,?] ! TaskError:
  original:Tensor[u32,2] = tensor([1,4294967295])
  return try batch(original)
fn main() -> u32:
  return 0
