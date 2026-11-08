error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:42}
  return value
fn main()->i64 ! TaskError:
  f=checked
  return try parallel:
    values=try await spawn map([0],fn(value):return f(value) catch fn(error):
      return match error:
        E.Bad {value}:value
    )
    values[0]
