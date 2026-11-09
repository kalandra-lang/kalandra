fn double(value:i64)->i64:
  return value*2
fn worker(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn main()->i64 ! TaskError:
  callback=worker
  values=try parallelMap([21],callback)
  return values[0]
