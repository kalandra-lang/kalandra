type Outcome:
  error:fn(int)->int
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn wrap(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn forward(f:fn(int)->int)->int ! TaskError:
  try parallel:
    try await spawn apply(42,f)
fn run(error:fn(int)->int)->int ! TaskError:
  try forward(wrap(error))
fn main()->int:
  42
