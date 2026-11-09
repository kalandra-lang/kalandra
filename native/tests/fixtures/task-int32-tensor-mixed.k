fn integers(values:Tensor[i32,?],other:Tensor[f32,?]) -> Tensor[i32,?]:
  changed = other + 0.5
  return values + 0
fn floating(other:Tensor[f32,?],values:Tensor[i32,?]) -> Tensor[f32,?]:
  unchanged = values + 0
  return other + 0.5
fn coordinator(values:Tensor[i32,?],other:Tensor[f32,?]) -> Tensor[i32,?] ! TaskError:
  return try parallel:
    a = spawn integers(values,other)
    b = spawn floating(other,values)
    first = try await a
    second = try await b
    first
fn probe() -> Tensor[i32,?] ! TaskError:
  values:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  other:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  return try parallel:
    work = spawn coordinator(values,other)
    try await work
fn floatingProbe() -> Tensor[f32,?] ! TaskError:
  values:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  other:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  return try parallel:
    a = spawn integers(values,other)
    b = spawn floating(other,values)
    first = try await a
    second = try await b
    second
fn main() -> i32:
  return 0
