error E:
  Bad:
    value:i64
error Other:
  Bad
fn descend(n:i64)->i64 ! (E | Other):
  if n==0:fail E.Bad {value:42}
  if n<0:fail Other.Bad
  return try descend(n-1)
fn main()->i64:
  callback=descend
  return callback(20) catch fn(error):
    return match error:
      E.Bad {value}:value
      Other.Bad:0
