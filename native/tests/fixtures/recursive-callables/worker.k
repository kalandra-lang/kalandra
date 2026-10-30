error E:
  Bad:
    text:String
    values:[int]
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad {text:"é"+"!",values:[20,20]}
  try descend(n-1)
fn worker(n:int)->int:
  callback=descend
  callback(n) catch fn(error):
    match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
fn main()->int ! TaskError:
  try parallel:
    task=spawn worker(20)
    first=try await task
    second=try await task
    first+second-42
