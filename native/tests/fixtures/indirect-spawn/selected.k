fn double(value:int)->int:
  value*2
fn choose(flag:Bool)->fn(int)->int:
  if flag:double
  else:fn(value:int):value+21
fn main()->int ! TaskError:
  f=choose(false)
  try parallel:
    try await spawn f(21)
