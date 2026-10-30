error E:
  Bad:
    callback:fn(int)->int ! TaskError
fn double(value:int)->int:
  value*2
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn raise()->int ! E:
  fail E.Bad {callback:run}
fn main()->int ! (E | TaskError):
  try parallel:
    try (await spawn raise() catch fn(error):
      match error:
        E.Bad {callback}:try callback(21)
        _:0
    )
