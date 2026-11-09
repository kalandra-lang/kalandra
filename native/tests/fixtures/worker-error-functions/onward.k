error E:
  Retry:
    callback:fn(i64)->i64
fn failer(offset:i64)->E ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:i64):return numbers[0]+value}
fn consume(error:E)->i64:
  return match error:
    E.Retry {callback}:callback(2)
fn recover()->E ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {callback}:E.Retry {callback:callback}
      _:fail error
fn run()->E ! (E | TaskError):
  return try parallel:
    try await spawn failer(40)
fn main()->i64 ! TaskError:
  error=try recover()
  return try parallel:
    try await spawn consume(error)
