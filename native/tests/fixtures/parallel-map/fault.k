fn divide(value:int)->int:
  1/value
fn run()->int ! TaskError:
  values=try parallelMap([1,0,2],divide)
  values[0]
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:fail error
      _:fail error
