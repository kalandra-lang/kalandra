fn identity(callback:fn(i64)->i64)->fn(i64)->i64:
  return callback
fn make(offset:i64)->fn(i64)->i64:
  return identity(fn(value):
    total=value+offset
    return total
  )
fn main()->i64:
  callback=make(20)
  return callback(22)
