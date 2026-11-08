error E:
  Bad
fn left(n:i64)->i64 ! E:
  if n==0:fail E.Bad
  return try right(n-1)
fn right(n:i64)->i64 ! E:
  if n==0:fail E.Bad
  return try left(n-1)
fn main()->i64:
  callback=left
  return callback(21) catch fn(error):return 42
