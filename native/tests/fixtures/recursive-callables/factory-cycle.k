error E:
  Bad
fn factory()->fn(int)->int ! E:
  descend
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad
  next=factory()
  try next(n-1)
fn main()->int:
  callback=factory()
  callback(20) catch fn(error):42
