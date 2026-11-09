error E:
  Retry:
    callback:fn(i64)->i64
fn failer(offset:i64)->i64 ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:i64):return numbers[0]+value}
fn worker(offset:i64)->(fn(i64)->i64) ! E:
  if offset==0:fail E.Retry {callback:fn(value:i64):return value}
  return fn(value:i64):return offset+value
fn main()->i64 ! (E | TaskError):
  callback=try parallel:
    try await spawn worker(40)
  return callback(2)
