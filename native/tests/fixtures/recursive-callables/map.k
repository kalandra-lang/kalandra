error E:
  Bad
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad
  try descend(n-1)
fn recover(n:int)->int:
  descend(n) catch fn(error):21
fn main()->int:
  values=map([10,20],recover)
  values[0]+values[1]
