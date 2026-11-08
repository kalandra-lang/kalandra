enum Choice:
  Some
fn even(n:i64)->bool:
  return match Choice.Some:
    Choice.Some:
      if n==0:
        true
      else:
        not even(n-1)
fn main()->i64:
  callback=even
  return if callback(20):42
  else:0
