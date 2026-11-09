fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn wrap(functions:[fn(i64)->i64])->fn(i64)->i64:
  return fn(value:i64):return functions[0](value)
fn producer()->fn(i64)->i64:
  values=[40]
  return wrap([fn(value:i64):return values[0]+value])
fn main()->i64 ! TaskError:
  f=try parallel:
    task=spawn producer()
    first=try await task
    second=try await task
    second
  return try parallel:
    try await spawn apply(2,f)
