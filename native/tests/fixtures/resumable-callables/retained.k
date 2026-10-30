fn add(value:int,offset:int)->int:
  value+offset
fn make(offset:int)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try parallel:
      try await spawn add(value,offset)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make(20)
  first=try f(1)
  second=try f(1)
  first+second
