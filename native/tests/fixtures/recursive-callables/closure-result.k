enum Choice:
  Some
fn descend(n:i64,base:i64)->fn(i64)->i64:
  return match Choice.Some:
    Choice.Some:
      if n==0:
        fn(value):return value+base
      else:
        descend(n-1,base+1)
fn main()->i64:
  callback=descend
  result=callback(20,20)
  return result(2)
