fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn main()->i64 ! TaskError:
  alias=identity
  functions=map([double],alias)
  return try parallel:
    try await spawn apply(21,functions[0])
