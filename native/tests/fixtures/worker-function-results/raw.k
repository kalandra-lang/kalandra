fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make(40)
  f(2)
