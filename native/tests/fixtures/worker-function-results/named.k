fn double(value:i64)->i64:
  return value*2
fn make()->fn(i64)->i64:
  return double
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make()
  return f(21)
