fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn apply(f:fn(i64)->i64 ! TaskError,value:i64)->i64 ! TaskError:
  return try f(value)
fn main()->i64 ! TaskError:
  return try apply(run,21)
