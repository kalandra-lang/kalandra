enum Choice:
  Some
fn descend(n:int)->[int]:
  match Choice.Some:
    Choice.Some:
      if n==0:
        [20,22]
      else:
        descend(n-1)
fn main()->int:
  callback=descend
  values=callback(20)
  values[0]+values[1]
