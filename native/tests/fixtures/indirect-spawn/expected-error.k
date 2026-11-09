fn double(value:i64)->i64:
  return value*2
error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:42}
  return value*2
fn run()->i64 ! (E | TaskError):
  f=checked
  return try parallel:
    try await spawn f(0)
fn main()->i64:
  return run() catch fn(error):
    return match error:
      E.Bad {value}:value
      _:0
