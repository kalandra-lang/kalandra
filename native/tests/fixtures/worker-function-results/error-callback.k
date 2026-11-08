error E:
  Bad:
    callback:fn(i64)->i64
fn double(value:i64)->i64:
  return value*2
fn make()->(fn(i64)->i64) ! E:
  fail E.Bad {callback:double}
fn run()->(fn(i64)->i64) ! (E | TaskError):
  return try parallel:
    try await spawn make()
fn main()->i64 ! TaskError:
  callback=try (run() catch fn(error):
    return match error:
      E.Bad {callback}:callback
      _:fail error
  )
  return callback(21)
