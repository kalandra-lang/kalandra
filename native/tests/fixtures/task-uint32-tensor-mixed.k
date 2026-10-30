fn unsigned(values:Tensor[uint32,?],signed:Tensor[int32,?],real:Tensor[float32,?]) -> Tensor[uint32,?]:
  integer = signed + 0
  floating = real + 0.5
  values + 0
fn signed(values:Tensor[uint32,?],integer:Tensor[int32,?],real:Tensor[float32,?]) -> Tensor[int32,?]:
  exact = values + 0
  floating = real + 0.5
  integer + 0
fn floating(real:Tensor[float32,?],values:Tensor[uint32,?],signed:Tensor[int32,?]) -> Tensor[float32,?]:
  exact = values + 0
  integer = signed + 0
  real + 0.5
fn coordinator(values:Tensor[uint32,?],integer:Tensor[int32,?],real:Tensor[float32,?]) -> Tensor[uint32,?] ! TaskError:
  try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    first
fn probe() -> Tensor[uint32,?] ! TaskError:
  values:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  try parallel:
    try await spawn coordinator(values,integer,real)
fn signedProbe() -> Tensor[int32,?] ! TaskError:
  values:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    second
fn floatingProbe() -> Tensor[float32,?] ! TaskError:
  values:Tensor[uint32,4] = tensor([0,4294967295,2147483648,16777217])
  integer:Tensor[int32,4] = tensor([-2147483648,2147483647,16777217,-5])
  real:Tensor[float32,4] = tensor([1.5,2.5,3.5,4.5])
  try parallel:
    a = spawn unsigned(values,integer,real)
    b = spawn signed(values,integer,real)
    c = spawn floating(real,values,integer)
    first = try await a
    second = try await b
    third = try await c
    third
fn main() -> uint32:
  0
