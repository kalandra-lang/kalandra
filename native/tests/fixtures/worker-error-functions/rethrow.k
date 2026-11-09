error E:
  Retry:
    callback:fn(i64)->i64
fn failer(offset:i64)->i64 ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:i64):return numbers[0]+value}
fn worker(offset:i64)->i64 ! E:
  return failer(offset) catch fn(error):fail error
fn run()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(40)
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {callback}:callback(2)
      _:fail error
