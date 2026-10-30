fn double(value:int)->int:
  value*2
error E:
  Bad:
    callback:fn(int)->int
fn failer()->int ! E:
  fail E.Bad {callback:double}
fn run()->int ! (E | TaskError):
  f=failer
  try parallel:
    try await spawn f()
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Bad {callback}:callback(21)
      _:fail error
