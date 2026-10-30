error E:
  Retry:
    callback:fn(int)->int
fn failer(value:int)->int ! E:
  numbers=[40]
  fail E.Retry {callback:fn(item:int):numbers[0]+item}
fn run()->int ! (E | TaskError):
  values=try parallelMap([0,1],failer)
  values[0]
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {callback}:callback(2)
      _:fail error
