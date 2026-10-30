fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  values=[20,20]
  f=fn(value:int):values[0]+values[1]+value
  try parallel:
    try await spawn apply(2,f)
