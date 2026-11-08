fn add(value:i64,offset:i64)->i64:
  return value+offset
fn make(offset:i64)->(fn(i64)->i64 ! TaskError):
  return fn(value:i64)->i64 ! TaskError:
    return try parallel:
      try await spawn add(value,offset)
fn main()->i64 ! TaskError:
  f=make(1)
  values=try parallelMap([19,21],f)
  return values[0]+values[1]
