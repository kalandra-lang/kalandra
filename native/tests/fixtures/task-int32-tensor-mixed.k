fn integers(values:Tensor[int32,?],other:Tensor[float32,?]) -> Tensor[int32,?]:
  changed = other + 0.5
  values + 0
fn floating(other:Tensor[float32,?],values:Tensor[int32,?]) -> Tensor[float32,?]:
  unchanged = values + 0
  other + 0.5
fn coordinator(values:Tensor[int32,?],other:Tensor[float32,?]) -> Tensor[int32,?] ! TaskError:
  try parallel:
    a = spawn integers(values,other)
    b = spawn floating(other,values)
    first = try await a
    second = try await b
    first
fn probe() -> Tensor[int32,?] ! TaskError:
  values:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  other:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  try parallel:
    work = spawn coordinator(values,other)
    try await work
fn floatingProbe() -> Tensor[float32,?] ! TaskError:
  values:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  other:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  try parallel:
    a = spawn integers(values,other)
    b = spawn floating(other,values)
    first = try await a
    second = try await b
    second
fn main() -> int32:
  0
