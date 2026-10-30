fn identity(callback:fn(int)->int)->fn(int)->int:
  callback
fn make(offset:int)->fn(int)->int:
  identity(fn(value):
    total=value+offset
    total
  )
fn main()->int:
  callback=make(20)
  callback(22)
