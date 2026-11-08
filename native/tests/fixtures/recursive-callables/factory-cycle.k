error E:
  Bad
fn factory()->fn(i64)->i64 ! E:
  return descend
fn descend(n:i64)->i64 ! E:
  if n==0:fail E.Bad
  next=factory()
  return try next(n-1)
fn main()->i64:
  callback=factory()
  return callback(20) catch fn(error):return 42
