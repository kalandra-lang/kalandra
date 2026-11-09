enum Choice:
  Some
fn descend(n:i64)->String:
  return match Choice.Some:
    Choice.Some:
      if n==0:
        "é"+"!"
      else:
        descend(n-1)
fn main()->i64:
  callback=descend
  return length(callback(20))+40
