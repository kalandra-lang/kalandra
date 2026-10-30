error E:
  Bad:
    text:String
    values:[int]
fn apply(value:int,f:fn(int)->int ! E)->int ! E:
  try f(value)
fn run()->int ! (E | TaskError):
  text="é"+"!"
  numbers=[20,20]
  f:fn(int)->int ! E=fn(value):
    if value==0:fail E.Bad {text:text,values:numbers}
    value
  try parallel:
    try await spawn apply(0,f)
fn main()->int:
  run() catch fn(error):
    match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      _:0
