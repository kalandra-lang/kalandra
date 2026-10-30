error E:
  Bad:
    value:int
fn run()->int ! (E | TaskError):
  values=try parallelMap([0],fn(value)->int ! E:
    if value==0:fail E.Bad {value:42}
    value
  )
  values[0]
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Bad {value}:value
      _:fail error
