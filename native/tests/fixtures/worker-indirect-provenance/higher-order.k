fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn make()->fn(i64)->i64:
  return double
fn invoke(factory:fn()->fn(i64)->i64)->fn(i64)->i64:
  return factory()
fn main()->i64 ! TaskError:
  f=invoke(make)
  return try parallel:
    try await spawn apply(21,f)
