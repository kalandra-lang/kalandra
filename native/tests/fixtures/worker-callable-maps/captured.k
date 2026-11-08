fn main()->i64 ! TaskError:
  offsets=[40]
  f=fn(value:i64):return offsets[0]+value
  return try parallel:
    values=try await spawn map([2],fn(value):return f(value))
    values[0]
