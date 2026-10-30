error E:
  Bad
fn checked()->int ! E:
  fail E.Bad
fn twice(value:int)->int:
  value*2
fn main()->int:
  twice(checked() catch fn(error):
    fallback:int=21
    fallback
  )
