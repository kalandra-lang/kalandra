fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  values=[40]
  functions=map([1],fn(offset):fn(value:int):values[0]+offset+value)
  try parallel:
    try await spawn apply(1,functions[0])
