fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(f:fn(int)->int)->int ! TaskError:
  try parallel:
    first=spawn apply(10,f)
    second=spawn apply(11,f)
    (try await first)+(try await second)
fn main()->int ! TaskError:
  try parallel:
    try await spawn forward(double)
