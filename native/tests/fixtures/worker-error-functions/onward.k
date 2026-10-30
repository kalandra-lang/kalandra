error E:
  Retry:
    callback:fn(int)->int
fn failer(offset:int)->E ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:int):numbers[0]+value}
fn consume(error:E)->int:
  match error:
    E.Retry {callback}:callback(2)
fn recover()->E ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callback}:E.Retry {callback:callback}
      _:fail error
fn run()->E ! (E | TaskError):
  try parallel:
    try await spawn failer(40)
fn main()->int ! TaskError:
  error=try recover()
  try parallel:
    try await spawn consume(error)
