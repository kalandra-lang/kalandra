fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64:
  return execute(value) catch fn(error):return 0
fn execute(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main()->i64:
  f=run
  return f(21)
