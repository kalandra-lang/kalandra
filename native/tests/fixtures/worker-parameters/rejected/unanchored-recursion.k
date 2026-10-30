fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn loop(f:fn(int)->int)->fn(int)->int:
  loop(f)
fn forward(f:fn(int)->int)->int ! TaskError:
  target=loop(f)
  try parallel:
    try await spawn apply(21,target)
fn main()->int:
  42
