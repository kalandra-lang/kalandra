error E:
  Bad
fn left(n:int)->int ! E:
  if n==0:fail E.Bad
  next=right
  try next(n-1)
fn right(n:int)->int ! E:
  if n==0:fail E.Bad
  next=left
  try next(n-1)
fn main()->int:
  callback=left
  callback(21) catch fn(error):42
