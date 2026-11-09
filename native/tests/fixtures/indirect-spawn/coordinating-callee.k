fn double(value:i64)->i64:
  return value*2
fn coordinate(value:i64)->i64:
  return run(value) catch fn(error):return 0
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main()->i64 ! TaskError:
  f=coordinate
  return try parallel:
    try await spawn f(21)
