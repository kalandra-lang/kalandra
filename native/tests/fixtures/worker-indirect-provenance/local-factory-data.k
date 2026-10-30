fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Outcome:
  error:TaskError
fn broken(value:int)->int:
  1/(value-value)
fn run()->(fn(int)->int) ! TaskError:
  try parallel:
    ignored=try await spawn broken(0)
    double
fn produce(error:TaskError)->fn(int)->int:
  outcome=Outcome {error:error}
  factory=fn(flag:Bool):
    ignored=match outcome.error:
      TaskError.Fault {kind}:1
      _:0
    double
  factory(true)
fn main()->int ! TaskError:
  f=run() catch fn(error):produce(error)
  try parallel:
    try await spawn apply(21,f)
