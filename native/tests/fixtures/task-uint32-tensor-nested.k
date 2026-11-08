fn shift(values:Tensor[u32,?,?],amount:u32) -> Tensor[u32,?,?]:
  return values + amount
fn nested(values:Tensor[u32,?,?]) -> Tensor[u32,?,?] ! TaskError:
  adjusted = values + 1
  return try parallel:
    first = spawn shift(adjusted,1)
    alias = first
    completed = try await first
    again = try await alias
    completed + (again - completed)
fn probe() -> Tensor[u32,?,?] ! TaskError:
  values:Tensor[u32,2,2] = tensor([[2147483648,10],[100,200]])
  return try parallel:
    work = spawn nested(values)
    try await work
fn main() -> u32:
  return 0
