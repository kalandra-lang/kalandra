fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn generic(callback:fn(i64)->i64 ! TaskError,value:i64)->i64 ! TaskError:
  return try callback(value)
fn main()->i64 ! TaskError:
  callback=run
  return try callback(21)
