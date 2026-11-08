fn apply(callback:fn(i64)->i64,value:i64)->i64:
  return callback(value)
fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64:
  return execute(value) catch fn(error):return 0
fn execute(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main()->i64:
  callback=run
  return callback(21)
