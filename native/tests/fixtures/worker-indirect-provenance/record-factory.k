fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Factory:
  make:fn(fn(i64)->i64)->fn(i64)->i64
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn main()->i64 ! TaskError:
  holder=Factory {make:identity}
  f=holder.make(double)
  return try parallel:
    try await spawn apply(21,f)
