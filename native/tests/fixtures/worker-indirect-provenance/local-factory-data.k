fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Outcome:
  error:TaskError
fn broken(value:i64)->i64:
  return 1/(value-value)
fn run()->(fn(i64)->i64) ! TaskError:
  return try parallel:
    ignored=try await spawn broken(0)
    double
fn produce(error:TaskError)->fn(i64)->i64:
  outcome=Outcome {error:error}
  factory=fn(flag:bool):
    ignored=match outcome.error:
      TaskError.Fault {kind}:1
      _:0
    return double
  return factory(true)
fn main()->i64 ! TaskError:
  f=run() catch fn(error):return produce(error)
  return try parallel:
    try await spawn apply(21,f)
