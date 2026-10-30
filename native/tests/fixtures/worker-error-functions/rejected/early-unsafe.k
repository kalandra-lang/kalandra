type Outcome:
  error:fn(int)->int
error E:
  Retry:
    callback:fn(int)->int
fn unsafe(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn worker(error:fn(int)->int,flag:Bool)->int ! E:
  if flag:fail E.Retry {callback:unsafe(error)}
  42
fn submit(error:fn(int)->int)->int ! (E | TaskError):
  try parallel:
    try await spawn worker(error,true)
fn main()->int:
  42
