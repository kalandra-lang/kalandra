fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  values=[40]
  make=fn(offset:int):fn(value:int):values[0]+offset+value
  f=make(1)
  try parallel:
    try await spawn apply(1,f)
