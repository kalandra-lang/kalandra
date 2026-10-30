fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(f:fn(int)->int)->int ! TaskError:
  try parallel:
    try await spawn apply(21,f)
fn main()->int ! TaskError:
  try forward(double)
