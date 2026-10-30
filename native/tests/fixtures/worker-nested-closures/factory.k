fn make(offset:int)->fn(int)->int:
  values=[offset]
  base=fn(value:int):values[0]+value
  fn(value:int):base(value)
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  first=make(10)
  second=make(20)
  try parallel:
    left=spawn apply(5,first)
    right=spawn apply(7,second)
    (try await left)+(try await right)
