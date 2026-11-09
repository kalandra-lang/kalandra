fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main(callback:fn(i64)->i64 ! TaskError)->i64 ! TaskError:
  known=run
  return try callback(21)
