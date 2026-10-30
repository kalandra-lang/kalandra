fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn make()->fn(int)->int:
  double
fn invoke(factory:fn()->fn(int)->int)->fn(int)->int:
  factory()
fn main()->int ! TaskError:
  f=invoke(make)
  try parallel:
    try await spawn apply(21,f)
