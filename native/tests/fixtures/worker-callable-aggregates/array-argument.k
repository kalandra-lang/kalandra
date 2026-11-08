fn apply(callbacks:[fn(i64)->i64])->i64:
  return callbacks[0](1)+callbacks[1](1)
fn main()->i64 ! TaskError:
  values=[20]
  f=fn(value:i64):return values[0]+value
  return try parallel:
    try await spawn apply([f,f])
