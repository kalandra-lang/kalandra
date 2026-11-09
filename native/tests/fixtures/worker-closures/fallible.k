error E:
  Bad:
    text:String
    values:[i64]
fn apply(value:i64,f:fn(i64)->i64 ! E)->i64 ! E:
  return try f(value)
fn run()->i64 ! (E | TaskError):
  text="é"+"!"
  numbers=[20,20]
  f:fn(i64)->i64 ! E=fn(value):
    if value==0:fail E.Bad {text:text,values:numbers}
    return value
  return try parallel:
    try await spawn apply(0,f)
fn main()->i64:
  return run() catch fn(error):
    return match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      _:0
