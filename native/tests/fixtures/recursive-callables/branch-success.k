enum Choice:
  Empty
  Some
fn count(n:i64)->i64:
  return match Choice.Some:
    Choice.Empty:0
    Choice.Some:
      if n==0:
        22
      else:
        count(n-1)+1
fn main()->i64:
  callback=count
  return callback(20)
