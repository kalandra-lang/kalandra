error E:
  Bad
fn count(n:i64)->i64:
  return if n==0:
    22
  else:
    count(n-1)+1
fn checked(n:i64)->i64 ! E:
  if n<0:fail E.Bad
  return count(n)
fn main()->i64:
  return checked(20) catch fn(error):return 0
