fn double(value:i64)->i64:
  return value*2
fn main()->i64:
  values=parallelMap([21],double)
  return values[0]
