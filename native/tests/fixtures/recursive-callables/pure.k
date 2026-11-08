fn count(n:i64)->i64:
  return if n==0:
    22
  else:
    count(n-1)+1
fn main()->i64:
  callback=count
  return callback(20)
