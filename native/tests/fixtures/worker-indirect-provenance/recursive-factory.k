fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn iterate(factory:fn(fn(int)->int)->fn(int)->int,f:fn(int)->int,count:int)->fn(int)->int:
  if count==0:f
  else:iterate(factory,factory(f),count-1)
fn main()->int ! TaskError:
  f=iterate(identity,double,3)
  try parallel:
    try await spawn apply(21,f)
