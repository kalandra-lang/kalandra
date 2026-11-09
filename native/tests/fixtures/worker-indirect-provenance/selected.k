fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn wrap(f:fn(i64)->i64)->fn(i64)->i64:
  return fn(value:i64):return f(value)
fn choose(flag:bool)->fn(fn(i64)->i64)->fn(i64)->i64:
  return if flag:identity
  else:wrap
fn main()->i64 ! TaskError:
  make=choose(false)
  f=make(double)
  return try parallel:
    try await spawn apply(21,f)
