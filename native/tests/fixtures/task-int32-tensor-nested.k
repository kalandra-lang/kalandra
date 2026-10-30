fn shift(values:Tensor[int32,?,?],amount:int32) -> Tensor[int32,?,?]:
  values + amount
fn nested(values:Tensor[int32,?,?]) -> Tensor[int32,?,?] ! TaskError:
  adjusted = values + 1
  try parallel:
    first = spawn shift(adjusted,1)
    alias = first
    completed = try await first
    again = try await alias
    completed + (again - completed)
fn probe() -> Tensor[int32,?,?] ! TaskError:
  values:Tensor[int32,2,2] = tensor([[16777217,-10],[100,200]])
  try parallel:
    work = spawn nested(values)
    try await work
fn main() -> int32:
  0
