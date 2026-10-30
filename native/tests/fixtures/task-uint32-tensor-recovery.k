fn overflow(values:Tensor[uint32,?]) -> Tensor[uint32,?]:
  values + 1
fn identity(values:Tensor[uint32,?]) -> Tensor[uint32,?]:
  values
fn batch(values:Tensor[uint32,?]) -> Tensor[uint32,?] ! TaskError:
  try parallel:
    work = spawn overflow(values)
    sibling = spawn identity(values)
    first = try await work
    try await sibling
    first
fn probe() -> Tensor[uint32,?] ! TaskError:
  original:Tensor[uint32,2] = tensor([1,4294967295])
  fallback:Tensor[uint32,2] = tensor([0,0])
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
fn failedProbe() -> Tensor[uint32,?] ! TaskError:
  original:Tensor[uint32,2] = tensor([1,4294967295])
  try batch(original)
fn main() -> uint32:
  0
