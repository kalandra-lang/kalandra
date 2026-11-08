error E:
  Retry:
    callbacks:[fn(i64)->i64]
fn failer()->i64 ! E:
  numbers=[40]
  fail E.Retry {callbacks:[fn(value:i64):return numbers[0]+value]}
fn run()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn failer()
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {callbacks}:callbacks[0](2)
      _:fail error
