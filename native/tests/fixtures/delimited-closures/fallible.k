error E:
  Bad
fn apply(callback:fn(int)->int ! E)->int:
  callback(0) catch fn(error):42
fn main()->int:
  apply(fn(value) ! E:
    if value==0:
      fail E.Bad
    value
  )
