fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  offset=40
  flag=true
  f=fn(value:int):
    if flag:value+offset
    else:0
  try parallel:
    try await spawn apply(2,f)
