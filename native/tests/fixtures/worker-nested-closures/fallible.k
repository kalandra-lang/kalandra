error E:
  Bad:
    text:String
    values:[int]
fn apply(value:int,f:fn(int)->int ! E)->int ! E:
  try f(value)
fn run()->int ! (E | TaskError):
  text="é!"
  values=[20,20]
  base:fn(int)->int ! E=fn(value):
    if value==0:fail E.Bad {text:text,values:values}
    value
  outer:fn(int)->int ! E=fn(value):try base(value)
  try parallel:
    try await spawn apply(0,outer)
fn main()->int:
  run() catch fn(error):
    match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      _:0
