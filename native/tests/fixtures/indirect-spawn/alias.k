fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  f=double
  return try parallel:
    try await spawn f(21)
