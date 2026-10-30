error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:42}
  value
fn main()->int ! TaskError:
  f=checked
  try parallel:
    values=try await spawn map([0],fn(value):f(value) catch fn(error):
      match error:
        E.Bad {value}:value
    )
    values[0]
