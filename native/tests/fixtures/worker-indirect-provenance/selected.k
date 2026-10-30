fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn wrap(f:fn(int)->int)->fn(int)->int:
  fn(value:int):f(value)
fn choose(flag:Bool)->fn(fn(int)->int)->fn(int)->int:
  if flag:identity
  else:wrap
fn main()->int ! TaskError:
  make=choose(false)
  f=make(double)
  try parallel:
    try await spawn apply(21,f)
