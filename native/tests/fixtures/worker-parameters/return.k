fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(f:fn(int)->int)->fn(int)->int:
  f
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn forward(double)
  f(21)
