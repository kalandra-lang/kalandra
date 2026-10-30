error E:
  Bad:
    values:[int]
fn make()->fn(int)->int ! E:
  values=[20,20]
  fn(value:int) ! E:
    if value==0:fail E.Bad {values:values}
    value
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make()
  f(0) catch fn(error):
    match error:
      E.Bad {values}:values[0]+values[1]+2
