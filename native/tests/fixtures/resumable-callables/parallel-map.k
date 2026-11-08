fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main()->i64 ! TaskError:
  f=run
  values=try parallelMap([10,11],f)
  return values[0]+values[1]
