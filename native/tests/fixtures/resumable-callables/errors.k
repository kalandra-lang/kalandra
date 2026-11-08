error E:
  Bad:
    value:i64
fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! (E | TaskError):
  if value==0:fail E.Bad {value:42}
  return try parallel:
    try await spawn double(value)
fn main()->i64:
  f=run
  return f(0) catch fn(error):
    return match error:
      E.Bad {value}:value
      _:0
