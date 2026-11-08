type Box:
  callback:fn(i64)->i64
fn main()->i64:
  offset:i64=20
  box=Box {callback:fn(value):
    total=value+offset
    return total
  }
  return box.callback(22)
