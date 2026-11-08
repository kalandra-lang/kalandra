fn add(value:i64,offset:i64)->i64:
  return value+offset
fn make(offset:i64)->(fn(i64)->i64 ! TaskError):
  return fn(value:i64)->i64 ! TaskError:
    return try parallel:
      try await spawn add(value,offset)
fn main()->i64 ! TaskError:
  callbacks=map([20,21],make)
  first=try callbacks[0](1)
  second=try callbacks[1](0)
  return first+second
