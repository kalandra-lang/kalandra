fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn make()->fn(i64)->i64:
  factory=identity
  return factory(double)
fn main()->i64 ! TaskError:
  f=try parallel:
    task=spawn make()
    first=try await task
    second=try await task
    second
  return try parallel:
    try await spawn apply(21,f)
