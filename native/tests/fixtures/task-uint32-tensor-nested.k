fn shift(values:Tensor[uint32,?,?],amount:uint32) -> Tensor[uint32,?,?]:
  values + amount
fn nested(values:Tensor[uint32,?,?]) -> Tensor[uint32,?,?] ! TaskError:
  adjusted = values + 1
  try parallel:
    first = spawn shift(adjusted,1)
    alias = first
    completed = try await first
    again = try await alias
    completed + (again - completed)
fn probe() -> Tensor[uint32,?,?] ! TaskError:
  values:Tensor[uint32,2,2] = tensor([[2147483648,10],[100,200]])
  try parallel:
    work = spawn nested(values)
    try await work
fn main() -> uint32:
  0
