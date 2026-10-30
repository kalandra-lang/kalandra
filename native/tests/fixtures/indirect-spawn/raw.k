fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  f=make(40)
  try parallel:
    try await spawn f(2)
