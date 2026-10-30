error E:
  Bad:
    value:int
fn double(value:int)->int:
  value*2
fn run(value:int)->int ! (E | TaskError):
  if value==0:fail E.Bad {value:42}
  try parallel:
    try await spawn double(value)
fn main()->int:
  f=run
  f(0) catch fn(error):
    match error:
      E.Bad {value}:value
      _:0
