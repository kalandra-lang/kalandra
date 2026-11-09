error E:
  Bad
fn apply(callback:fn(i64)->i64 ! E)->i64:
  return callback(0) catch fn(error):return 42
fn main()->i64:
  return apply(fn(value) ! E:
    if value==0:
      fail E.Bad
    return value
  )
