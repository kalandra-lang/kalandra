fn unsigned(values:Tensor[u32,?],signed:Tensor[i32,?],real:Tensor[f32,?]) -> Tensor[u32,?]:
  integer = signed + 0
  floating = real + 0.5
  return values + 0
fn signed(values:Tensor[u32,?],integer:Tensor[i32,?],real:Tensor[f32,?]) -> Tensor[i32,?]:
  exact = values + 0
  floating = real + 0.5
  return integer + 0
fn floating(real:Tensor[f32,?],values:Tensor[u32,?],signed:Tensor[i32,?]) -> Tensor[f32,?]:
  exact = values + 0
  integer = signed + 0
  return real + 0.5
fn coordinator(values:Tensor[u32,?],integer:Tensor[i32,?],real:Tensor[f32,?]) -> Tensor[u32,?] ! TaskError:
  return try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    first
fn probe() -> Tensor[u32,?] ! TaskError:
  values:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  return try parallel:
    try await spawn coordinator(values,integer,real)
fn signedProbe() -> Tensor[i32,?] ! TaskError:
  values:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  return try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    second
fn floatingProbe() -> Tensor[f32,?] ! TaskError:
  values:Tensor[u32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[i32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[f32,4] = tensor([1.5,2.5,3.5,4.5])
  return try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    third
fn main() -> u32:
  return 0
