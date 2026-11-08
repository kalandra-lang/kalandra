fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  values=[20,20]
  base=fn(value:i64):return values[0]+values[1]+value
  outer=fn(value:i64):return base(value)
  return try parallel:
    try await spawn apply(2,outer)
