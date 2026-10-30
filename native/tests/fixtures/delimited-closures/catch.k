error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:21}
  value
fn main()->int:
  values=map([0,21],fn(value):
    checked(value) catch fn(error):
      match error:
        E.Bad {value}:value
  )
  values[0]+values[1]
