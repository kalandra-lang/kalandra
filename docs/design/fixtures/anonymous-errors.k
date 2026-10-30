error E:
  Bad:
    value:int
export fn main()->int:
  offset:int=42
  validate=fn(value:int)->int ! E:
    if value<0:
      fail E.Bad {value:offset}
    value+offset
  validate(-1) catch fn(error):
    match error:
      E.Bad {value}:value
