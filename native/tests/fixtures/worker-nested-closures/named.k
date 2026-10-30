fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  target=double
  f=fn(value:int):target(value)+2
  try parallel:
    try await spawn apply(20,f)
