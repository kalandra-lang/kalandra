fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  values=try parallelMap([10,11],fn(value)->i64 ! TaskError:
    inner=try parallelMap([value],double)
    return inner[0]
  )
  return values[0]+values[1]
