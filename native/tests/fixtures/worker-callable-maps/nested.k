fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn worker()->int ! TaskError:
  f=make(40)
  try parallel:
    values=try await spawn map([2],f)
    values[0]
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker()
