fn leaf(value:i64)->i64:
  return value+42
fn run(value:i64)->i64 ! TaskError:
  return if value==0:
    try parallel:
      try await spawn leaf(value)
  else:
    f=run
    try f(value-1)
fn main()->i64 ! TaskError:
  f=run
  return try f(8)
