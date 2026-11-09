error E:
  Retry:
    callback:fn(i64)->i64
fn worker(callback:fn(i64)->i64)->i64 ! E:
  fail E.Retry {callback:callback}
fn submit(callback:fn(i64)->i64)->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(callback)
fn main()->i64:
  return 42
