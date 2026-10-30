fn double(value:int)->int:
  value*2
fn make()->fn(int)->int:
  values=[40]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  f=make()
  try parallel:
    task=spawn f(2)
    first=try await task
    second=try await task
    second
