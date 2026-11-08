error E:
  Bad:
    value:i64
fn descend(n:i64)->i64 ! E:
  if n==0:fail E.Bad {value:42}
  return try descend(n-1)
fn main()->i64:
  callback=descend
  return callback(20) catch fn(error):
    return match error:
      E.Bad {value}:value
