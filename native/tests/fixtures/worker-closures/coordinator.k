fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int ! TaskError:
  try parallel:
    work=spawn double(value)
    a=try await work
    f(a)
fn main()->int ! TaskError:
  values=[20,20]
  f=fn(value:int):value+values[0]+values[1]
  try parallel:
    try await spawn apply(1,f)
