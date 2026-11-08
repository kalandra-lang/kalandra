fn apply(callback:fn(fn(i64)->i64,i64)->i64)->i64:
  return callback(fn(value:i64):return value*2,21)
fn main()->i64:
  return apply(fn(
    callback:fn(i64)->i64,
    value:
           i64
  )->i64:
    total=callback(value)
    return total
  )
