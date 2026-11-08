fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  values=try ([21] -> parallelMap(double))
  return values[0]
