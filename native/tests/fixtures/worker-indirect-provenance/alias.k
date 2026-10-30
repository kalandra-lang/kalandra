fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn main()->int ! TaskError:
  alias=identity
  f=alias(double)
  try parallel:
    try await spawn apply(21,f)
