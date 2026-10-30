fn child(value:int)->int:
  value
fn apply(value:int,f:fn(int)->int)->int ! TaskError:
  try parallel:
    input=try await spawn child(value)
    f(input)
fn main()->int ! TaskError:
  values=[40]
  base=fn(value:int):values[0]+value
  outer=fn(value:int):base(value)
  try parallel:
    try await spawn apply(2,outer)
