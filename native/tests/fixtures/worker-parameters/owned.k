fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn wrap(functions:[fn(int)->int])->fn(int)->int:
  fn(value:int):functions[0](value)
fn producer()->fn(int)->int:
  values=[40]
  wrap([fn(value:int):values[0]+value])
fn main()->int ! TaskError:
  f=try parallel:
    task=spawn producer()
    first=try await task
    second=try await task
    second
  try parallel:
    try await spawn apply(2,f)
