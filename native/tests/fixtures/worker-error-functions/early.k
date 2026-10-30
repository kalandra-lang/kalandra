error E:
  Retry:
    callback:fn(int)->int
fn failer(offset:int)->int ! E:
  numbers=[offset]
  if offset==40:fail E.Retry {callback:fn(value:int):numbers[0]+value}
  offset
fn run()->int ! (E | TaskError):
  try parallel:
    try await spawn failer(40)
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callback}:callback(2)
      _:fail error
