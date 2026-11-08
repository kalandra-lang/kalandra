fn double(value:i64)->i64:
  return value*2
fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  return fn(value:i64):return values[0]+value
fn main()->i64 ! TaskError:
  factory=make
  f=try parallel:
    task=spawn factory(40)
    first=try await task
    second=try await task
    second
  return try parallel:
    try await spawn f(2)
