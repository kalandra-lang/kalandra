fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn forward(f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    first=spawn apply(10,f)
    second=spawn apply(11,f)
    (try await first)+(try await second)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn forward(double)
