fn add(value:int,offset:int)->int:
  value+offset
fn make(offset:int)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try parallel:
      try await spawn add(value,offset)
fn main()->int ! TaskError:
  callbacks=map([20,21],make)
  first=try callbacks[0](1)
  second=try callbacks[1](0)
  first+second
