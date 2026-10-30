fn make(offset:int)->fn(int)->int:
  values=[offset]
  base=fn(value:int):values[0]+value
  fn(value:int):base(value)
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make(40)
  try parallel:
    try await spawn apply(2,f)
