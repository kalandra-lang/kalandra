error E:
  Retry:
    callback:fn(i64)->i64
fn failer(value:i64)->i64 ! E:
  numbers=[40]
  fail E.Retry {callback:fn(item:i64):return numbers[0]+item}
fn run()->i64 ! (E | TaskError):
  values=try parallelMap([0,1],failer)
  return values[0]
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {callback}:callback(2)
      _:fail error
