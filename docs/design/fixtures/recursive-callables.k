error E:
  Bad:
    value:int
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad {value:42}
  try descend(n-1)
fn main()->int:
  callback=descend
  callback(20) catch fn(error):
    match error:
      E.Bad {value}:value
