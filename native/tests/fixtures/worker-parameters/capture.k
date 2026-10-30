fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn wrap(f:fn(int)->int)->fn(int)->int:
  fn(value:int):f(value)
fn main()->int ! TaskError:
  f=wrap(double)
  try parallel:
    try await spawn apply(21,f)
