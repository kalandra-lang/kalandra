fn apply(callback:fn(i64)->i64,value:i64)->i64:
  return callback(value)
fn main()->i64:
  return apply(
    fn(value):
      adjusted=value+1
      return adjusted*2
    ,20
  )
