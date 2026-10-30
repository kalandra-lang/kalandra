fn double(value:int)->int:
  value*2
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn make()->fn(int)->int:
  values=[double]
  fn(value:int):values[0](value)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make()
  f(21)
