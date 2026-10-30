fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(f:fn(int)->int)->fn(int)->int:
  alias=f
  if true:alias
  else:double
fn main()->int ! TaskError:
  first=forward(double)
  second=forward(fn(value:int):value+21)
  try parallel:
    a=spawn apply(21,first)
    b=spawn apply(21,second)
    ((try await a)+(try await b))/2
