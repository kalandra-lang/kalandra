fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  values=[40]
  make=fn(offset:i64):return fn(value:i64):return values[0]+offset+value
  f=make(1)
  return try parallel:
    try await spawn apply(1,f)
