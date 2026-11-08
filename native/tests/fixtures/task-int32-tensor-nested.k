fn shift(values:Tensor[i32,?,?],amount:i32) -> Tensor[i32,?,?]:
  return values + amount
fn nested(values:Tensor[i32,?,?]) -> Tensor[i32,?,?] ! TaskError:
  adjusted = values + 1
  return try parallel:
    first = spawn shift(adjusted,1)
    alias = first
    completed = try await first
    again = try await alias
    completed + (again - completed)
fn probe() -> Tensor[i32,?,?] ! TaskError:
  values:Tensor[i32,2,2] = tensor([[16777217,-10],[100,200]])
  return try parallel:
    work = spawn nested(values)
    try await work
fn main() -> i32:
  return 0
