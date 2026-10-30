enum Choice:
  Some
fn descend(n:int)->String:
  match Choice.Some:
    Choice.Some:
      if n==0:
        "é"+"!"
      else:
        descend(n-1)
fn main()->int:
  callback=descend
  length(callback(20))+40
