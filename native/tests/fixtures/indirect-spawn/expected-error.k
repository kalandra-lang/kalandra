fn double(value:int)->int:
  value*2
error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:42}
  value*2
fn run()->int ! (E | TaskError):
  f=checked
  try parallel:
    try await spawn f(0)
fn main()->int:
  run() catch fn(error):
    match error:
      E.Bad {value}:value
      _:0
