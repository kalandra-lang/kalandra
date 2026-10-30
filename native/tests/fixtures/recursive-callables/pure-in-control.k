error E:
  Bad
fn count(n:int)->int:
  if n==0:
    22
  else:
    count(n-1)+1
fn checked(n:int)->int ! E:
  if n<0:fail E.Bad
  count(n)
fn main()->int:
  checked(20) catch fn(error):0
