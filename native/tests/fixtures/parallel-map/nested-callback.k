fn double(value:i64)->i64:
  return value*2
fn worker(value:i64)->i64 ! TaskError:
  result=try parallelMap([value],double)
  return result[0]
fn main()->i64 ! TaskError:
  values=try parallelMap([10,11],worker)
  return values[0]+values[1]
