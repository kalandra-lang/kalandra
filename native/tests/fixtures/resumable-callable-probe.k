fn add(value:int,offset:int)->int:
  value+offset
fn make(offset:int)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try parallel:
      try await spawn add(value,offset)
fn main()->int ! TaskError:
  f=make(1)
  values=try parallelMap([19,21],f)
  values[0]+values[1]
