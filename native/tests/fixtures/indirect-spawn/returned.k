fn double(value:int)->int:
  value*2
fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  factory=make
  f=try parallel:
    task=spawn factory(40)
    first=try await task
    second=try await task
    second
  try parallel:
    try await spawn f(2)
