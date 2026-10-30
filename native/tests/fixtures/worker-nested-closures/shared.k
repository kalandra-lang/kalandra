fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  numbers=[20]
  base=fn(value:int):numbers[0]+value
  left=base
  right=base
  outer=fn(value:int):left(value)+right(value)
  try parallel:
    try await spawn apply(1,outer)
