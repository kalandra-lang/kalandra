error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:42}
  return value
fn run()->i64 ! (E | TaskError):
  callback=checked
  values=try parallelMap([1,0,3],callback)
  return values[0]
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Bad {value}:value
      _:fail error
