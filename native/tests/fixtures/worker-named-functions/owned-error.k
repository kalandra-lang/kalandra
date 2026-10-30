error E:
  Bad:
    text:String
    values:[int]
fn checked(value:int)->int ! E:
  fail E.Bad {text:"é"+"!",values:[value,20]}
fn apply(value:int,transform:fn(int)->int ! E)->int ! E:
  try transform(value)
fn run()->int ! (E | TaskError):
  try parallel:
    work=spawn apply(20,checked)
    try await work
fn main()->int:
  run() catch fn(error):
    match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      _:0
