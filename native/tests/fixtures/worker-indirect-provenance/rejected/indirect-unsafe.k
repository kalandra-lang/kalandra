fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Outcome:
  error:fn(int)->int
fn unsafe(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn identity(f:fn(int)->int)->fn(int)->int:
  f
fn consume(error:fn(int)->int)->int ! TaskError:
  factory=identity
  f=factory(unsafe(error))
  try parallel:
    try await spawn apply(42,f)
fn main()->int:
  42
