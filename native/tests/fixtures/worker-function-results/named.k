fn double(value:int)->int:
  value*2
fn make()->fn(int)->int:
  double
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make()
  f(21)
