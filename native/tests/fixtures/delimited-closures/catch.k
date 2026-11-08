error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:21}
  return value
fn main()->i64:
  values=map([0,21],fn(value):
    return checked(value) catch fn(error):
      return match error:
        E.Bad {value}:value
  )
  return values[0]+values[1]
