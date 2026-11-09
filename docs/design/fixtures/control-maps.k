error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:21}
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):
    return match error:
      E.Bad {value}:value
fn main()->i64:
  result=map([0,21],recover)
  return result[0]+result[1]
