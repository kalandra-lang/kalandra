error E:
  Bad
fn left(n:int)->int ! E:
  if n==0:fail E.Bad
  try right(n-1)
fn right(n:int)->int ! E:
  if n==0:fail E.Bad
  try left(n-1)
fn main()->int:
  callback=left
  callback(21) catch fn(error):42
