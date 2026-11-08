error E:
  Bad:
    callback:fn(i64)->i64 ! TaskError
fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn raise()->i64 ! E:
  fail E.Bad {callback:run}
fn main()->i64 ! (E | TaskError):
  return try parallel:
    try (await spawn raise() catch fn(error):
      return match error:
        E.Bad {callback}:try callback(21)
        _:0
    )
