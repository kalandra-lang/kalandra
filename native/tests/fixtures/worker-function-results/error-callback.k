error E:
  Bad:
    callback:fn(int)->int
fn double(value:int)->int:
  value*2
fn make()->(fn(int)->int) ! E:
  fail E.Bad {callback:double}
fn run()->(fn(int)->int) ! (E | TaskError):
  try parallel:
    try await spawn make()
fn main()->int ! TaskError:
  callback=try (run() catch fn(error):
    match error:
      E.Bad {callback}:callback
      _:fail error
  )
  callback(21)
