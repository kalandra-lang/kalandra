error E:
  Bad:
    value:i64
export fn main()->i64:
  offset:i64=42
  validate=fn(value:i64)->i64 ! E:
    if value<0:
      fail E.Bad {value:offset}
    return value+offset
  return validate(-1) catch fn(error):
    return match error:
      E.Bad {value}:value
