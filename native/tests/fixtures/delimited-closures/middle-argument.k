fn apply(base:i64,callback:fn(i64)->i64,value:i64)->i64:
  return callback(value)+base
fn main()->i64:
  return apply(2,fn(value):
    adjusted=value+1
    return adjusted*2
  ,19)
