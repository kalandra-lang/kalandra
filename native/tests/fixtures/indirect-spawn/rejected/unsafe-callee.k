type Outcome:
  error:fn(int)->int
fn run(error:fn(int)->int)->int ! TaskError:
  outcome=Outcome {error:error}
  f=fn(value:int):
    outcome.error(value)
  try parallel:
    try await spawn f(42)
fn main()->int:
  42
