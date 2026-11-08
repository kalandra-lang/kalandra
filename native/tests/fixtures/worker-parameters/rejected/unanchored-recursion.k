fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn loop(f:fn(i64)->i64)->fn(i64)->i64:
  return loop(f)
fn forward(f:fn(i64)->i64)->i64 ! TaskError:
  target=loop(f)
  return try parallel:
    try await spawn apply(21,target)
fn main()->i64:
  return 42
