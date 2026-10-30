error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:42}
  value
fn run()->int ! (E | TaskError):
  callback=checked
  values=try parallelMap([1,0,3],callback)
  values[0]
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Bad {value}:value
      _:fail error
