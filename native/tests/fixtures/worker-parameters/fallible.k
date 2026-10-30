error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:42}
  value*2
fn apply(value:int,f:fn(int)->int ! E)->int ! E:
  try f(value)
fn forward(f:fn(int)->int ! E)->int ! (E | TaskError):
  try parallel:
    try await spawn apply(0,f)
fn main()->int:
  forward(checked) catch fn(error):
    match error:
      E.Bad {value}:value
      _:0
