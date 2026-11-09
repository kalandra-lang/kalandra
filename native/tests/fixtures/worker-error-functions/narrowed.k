error E:
  Retry:
    callback:fn(i64)->i64
fn failer(offset:i64)->i64 ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:i64):return numbers[0]+value}
error Other:
  No
fn mixed(offset:i64)->i64 ! (E | Other):
  return try failer(offset)
fn worker(offset:i64)->i64 ! E:
  return mixed(offset) catch fn(error):
    return match error:
      E.Retry:fail error
      Other.No:0
fn run()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(40)
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {callback}:callback(2)
      _:fail error
