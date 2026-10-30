fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Outcome:
  error:TaskError
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn unused(error:TaskError)->int:
  outcome=Outcome {error:error}
  other=fn(f:fn(int)->int):
    fn(value:int):
      match outcome.error:
        TaskError.Fault {kind}:f(value)
        _:0
  0
fn main()->int ! TaskError:
  factory=identity
  safe=factory(double)
  try parallel:
    try await spawn apply(21,safe)
