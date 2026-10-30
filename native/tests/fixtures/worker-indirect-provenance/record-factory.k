fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Factory:
  make:fn(fn(int)->int)->fn(int)->int
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn main()->int ! TaskError:
  holder=Factory {make:identity}
  f=holder.make(double)
  try parallel:
    try await spawn apply(21,f)
