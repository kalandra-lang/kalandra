fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn worker()->i64 ! TaskError:
  f=make(40)
  return try parallel:
    values=try await spawn map([2],f)
    values[0]
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker()
