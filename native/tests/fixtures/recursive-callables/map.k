error E:
  Bad
fn descend(n:i64)->i64 ! E:
  if n==0:fail E.Bad
  return try descend(n-1)
fn recover(n:i64)->i64:
  return descend(n) catch fn(error):return 21
fn main()->i64:
  values=map([10,20],recover)
  return values[0]+values[1]
