error E:
  Bad:
    values:[i64]
fn make()->fn(i64)->i64 ! E:
  values=[20,20]
  return fn(value:i64) ! E:
    if value==0:fail E.Bad {values:values}
    return value
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make()
  return f(0) catch fn(error):
    return match error:
      E.Bad {values}:values[0]+values[1]+2
