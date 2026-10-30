fn add(value:int,offset:int)->int:
  value+offset
fn make(offset:int)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try parallel:
      try await spawn add(value,offset)
fn wrap(f:fn(int)->int ! TaskError)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try f(value)
fn main()->int ! TaskError:
  f=wrap(make(20))
  try parallel:
    try await spawn f(22)
