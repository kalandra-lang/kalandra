error E:
  Retry:
    callbacks:[fn(int)->int]
fn failer()->int ! E:
  numbers=[40]
  fail E.Retry {callbacks:[fn(value:int):numbers[0]+value]}
fn run()->int ! (E | TaskError):
  try parallel:
    try await spawn failer()
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callbacks}:callbacks[0](2)
      _:fail error
