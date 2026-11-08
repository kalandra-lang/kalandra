error E:
  Bad
fn checked()->i64 ! E:
  fail E.Bad
fn twice(value:i64)->i64:
  return value*2
fn main()->i64:
  return twice(checked() catch fn(error):
    fallback:i64=21
    return fallback
  )
