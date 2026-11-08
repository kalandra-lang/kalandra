fn child(value:i64)->i64:
  return value
fn apply(value:i64,f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    input=try await spawn child(value)
    f(input)
fn main()->i64 ! TaskError:
  values=[40]
  base=fn(value:i64):return values[0]+value
  outer=fn(value:i64):return base(value)
  return try parallel:
    try await spawn apply(2,outer)
