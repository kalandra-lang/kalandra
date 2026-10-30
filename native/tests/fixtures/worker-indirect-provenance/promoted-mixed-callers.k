fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn make()->fn(int)->int:
  double
fn main()->int ! TaskError:
  safe=identity(double)
  maker=make
  other=identity(maker())
  try parallel:
    try await spawn apply(21,safe)
