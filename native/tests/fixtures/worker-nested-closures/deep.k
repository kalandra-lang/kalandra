fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  values=[20,20]
  base=fn(value:int):values[0]+values[1]+value
  middle=fn(value:int):base(value)
  outer=fn(value:int):middle(value)
  try parallel:
    work=spawn apply(2,outer)
    first=try await work
    second=try await work
    first+second-42
