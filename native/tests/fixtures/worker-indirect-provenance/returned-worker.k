fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn make()->fn(int)->int:
  factory=identity
  factory(double)
fn main()->int ! TaskError:
  f=try parallel:
    task=spawn make()
    first=try await task
    second=try await task
    second
  try parallel:
    try await spawn apply(21,f)
