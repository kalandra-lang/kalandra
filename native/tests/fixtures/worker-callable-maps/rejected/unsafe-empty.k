type Outcome:
  error:fn(int)->int
fn unsafe(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn consume(error:fn(int)->int)->int ! TaskError:
  input:[int]=[]
  f=unsafe(error)
  try parallel:
    values=try await spawn map(input,f)
    length(values)
fn main()->int:
  42
