fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Outcome:
  error:TaskError
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn unused(error:TaskError)->i64:
  outcome=Outcome {error:error}
  other=fn(f:fn(i64)->i64):
    return fn(value:i64):
      return match outcome.error:
        TaskError.Fault {kind}:f(value)
        _:0
  return 0
fn main()->i64 ! TaskError:
  factory=identity
  safe=factory(double)
  return try parallel:
    try await spawn apply(21,safe)
