error E:
  Bad:
    text:String
    values:[i64]
fn checked(value:i64)->i64 ! E:
  fail E.Bad {text:"é"+"!",values:[value,20]}
fn apply(value:i64,transform:fn(i64)->i64 ! E)->i64 ! E:
  return try transform(value)
fn run()->i64 ! (E | TaskError):
  return try parallel:
    work=spawn apply(20,checked)
    try await work
fn main()->i64:
  return run() catch fn(error):
    return match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      _:0
