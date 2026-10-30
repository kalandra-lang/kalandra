fn double(value:int)->int:
  value*2
fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  try parallel:
    try await spawn make(40)(2)
