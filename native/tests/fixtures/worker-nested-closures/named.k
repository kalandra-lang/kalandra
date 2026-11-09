fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  target=double
  f=fn(value:i64):return target(value)+2
  return try parallel:
    try await spawn apply(20,f)
