fn double(value:int)->int:
  value*2
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
type Box:
  f:fn(int)->int
fn main()->int ! TaskError:
  box=Box {f:double}
  f=fn(value:int):box.f(value)
  try parallel:
    try await spawn apply(21,f)
