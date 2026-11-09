error E:
  Bad:
    value:i64
fn run()->i64 ! (E | TaskError):
  values=try parallelMap([0],fn(value)->i64 ! E:
    if value==0:fail E.Bad {value:42}
    return value
  )
  return values[0]
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Bad {value}:value
      _:fail error
