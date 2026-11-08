fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  values=[40]
  f=fn(value:i64):return values[0]+value
  return try parallel:
    try await spawn f(2)
