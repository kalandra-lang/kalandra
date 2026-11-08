fn double(value:i64)->i64:
  return value*2
fn make()->fn(i64)->i64:
  values=[40]
  return fn(value:i64):return values[0]+value
fn main()->i64 ! TaskError:
  f=make()
  return try parallel:
    task=spawn f(2)
    first=try await task
    second=try await task
    second
