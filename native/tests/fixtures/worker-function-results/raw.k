fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  return fn(value:i64):return values[0]+value
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make(40)
  return f(2)
