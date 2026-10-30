type Row:
  text:String
  values:[int]
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  rows=[Row {text:"é"+"!",values:[20,20]}]
  f=fn(value:int):length(rows[0].text)+rows[0].values[0]+rows[0].values[1]+value
  try parallel:
    work=spawn apply(0,f)
    a=try await work
    b=try await work
    a+b-42
