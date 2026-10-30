enum Choice:
  Some
fn even(n:int)->Bool:
  match Choice.Some:
    Choice.Some:
      if n==0:
        true
      else:
        not even(n-1)
fn main()->int:
  callback=even
  if callback(20):42
  else:0
