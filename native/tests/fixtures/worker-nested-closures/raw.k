fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  values=[20,20]
  base=fn(value:int):values[0]+values[1]+value
  outer=fn(value:int):base(value)
  try parallel:
    try await spawn apply(2,outer)
