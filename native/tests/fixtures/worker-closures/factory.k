fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn make(offset:int)->fn(int)->int:
  values=[offset]
  fn(value:int):values[0]+value
fn main()->int ! TaskError:
  first=make(40)
  second=make(20)
  try parallel:
    a=spawn apply(2,first)
    b=spawn apply(22,second)
    x=try await a
    y=try await b
    x+y-42
