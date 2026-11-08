fn double(value:i64)->i64:
  return value*2
fn answer()->i64:
  return 42
fn main()->i64 ! TaskError:
  f=answer
  return try parallel:
    try await spawn f()
