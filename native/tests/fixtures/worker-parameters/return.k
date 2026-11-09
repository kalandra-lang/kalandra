fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn forward(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn forward(double)
  return f(21)
