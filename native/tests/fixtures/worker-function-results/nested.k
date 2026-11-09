fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  base=fn(value:i64):return values[0]+value
  return fn(value:i64):return base(value)
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make(40)
  return try parallel:
    try await spawn apply(2,f)
