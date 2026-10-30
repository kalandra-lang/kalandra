error E:
  Retry:
    callback:fn(int)->int
fn failer(offset:int)->int ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:int):numbers[0]+value}
fn run()->int ! (E | TaskError):
  f=failer
  try parallel:
    try await spawn f(40)
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callback}:callback(2)
      _:fail error
