type Box:
  callback:fn(int)->int
fn main()->int:
  offset:int=20
  box=Box {callback:fn(value):
    total=value+offset
    total
  }
  box.callback(22)
