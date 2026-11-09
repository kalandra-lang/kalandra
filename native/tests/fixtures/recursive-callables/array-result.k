enum Choice:
  Some
fn descend(n:i64)->[i64]:
  return match Choice.Some:
    Choice.Some:
      if n==0:
        [20,22]
      else:
        descend(n-1)
fn main()->i64:
  callback=descend
  values=callback(20)
  return values[0]+values[1]
