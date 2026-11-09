fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    work=spawn double(value)
    a=try await work
    f(a)
fn main()->i64 ! TaskError:
  values=[20,20]
  f=fn(value:i64):return value+values[0]+values[1]
  return try parallel:
    try await spawn apply(1,f)
