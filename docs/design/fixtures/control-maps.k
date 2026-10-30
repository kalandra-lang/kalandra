error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:21}
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):
    match error:
      E.Bad {value}:value
fn main()->int:
  result=map([0,21],recover)
  result[0]+result[1]
