type Outcome:
  error:TaskError
fn identity(value:int)->int:
  value
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn identity(value)
fn capture(error:TaskError)->(fn(int)->int ! TaskError):
  outcome=Outcome {error:error}
  fn(value:int)->int ! TaskError:
    selected=match outcome.error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:value
          _:0
      _:0
    try parallel:
      try await spawn identity(selected)
fn failer(divisor:int)->(fn(int)->int ! TaskError):
  ignored=1/divisor
  run
fn factory()->(fn(int)->int ! TaskError) ! TaskError:
  try parallel:
    try await spawn failer(0)
fn main()->int ! TaskError:
  callback=factory() catch fn(error):capture(error)
  try callback(42)
