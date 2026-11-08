fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn wrap(f:fn(i64)->i64)->fn(i64)->i64:
  return fn(value:i64):return f(value)
fn main()->i64 ! TaskError:
  f=wrap(double)
  return try parallel:
    try await spawn apply(21,f)
