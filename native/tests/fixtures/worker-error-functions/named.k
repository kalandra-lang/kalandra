fn double(value:i64)->i64:
  return value*2
error E:
  Bad:
    callback:fn(i64)->i64
fn failer()->i64 ! E:
  fail E.Bad {callback:double}
fn run()->i64 ! (E | TaskError):
  f=failer
  return try parallel:
    try await spawn f()
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Bad {callback}:callback(21)
      _:fail error
