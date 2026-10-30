fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn forward(f:fn(int)->int,count:int)->fn(int)->int:
  if count==0:f
  else:forward(f,count-1)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn forward(double,3)
  f(21)
