fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn coordinate(offset:int)->(fn(int)->int) ! TaskError:
  try parallel:
    try await spawn make(offset)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn coordinate(40)
  f(2)
