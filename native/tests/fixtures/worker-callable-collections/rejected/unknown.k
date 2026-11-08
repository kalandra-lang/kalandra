fn consume(callbacks:[fn(i64)->i64])->i64 ! TaskError:
  return try parallel:
    try await spawn length(callbacks)
fn main()->i64:
  return 42
