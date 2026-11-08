error E:
  Retry:
    callbacks:[fn(i64)->i64]
fn worker()->i64 ! E:
  return 42
fn main()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker()
