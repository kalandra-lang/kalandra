fn divide(value:i64)->i64:
  return 1/value
fn run()->i64 ! TaskError:
  values=try parallelMap([1,0,2],divide)
  return values[0]
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:fail error
      _:fail error
